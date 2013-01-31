// Copyright (c) 2013 The Trustees of Indiana University and Indiana
//                    University Research and Technology
//                    Corporation.  All rights reserved.
//  Copyright (c) 2007-2012 Hartmut Kaiser
//  Copyright (c) 2007 Richard D Guidry Jr
//  Copyright (c) 2011 Bryce Lelbach
//  Copyright (c) 2011 Katelyn Kufahl
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_fwd.hpp>
#include <hpx/exception_list.hpp>
#include <hpx/runtime/naming/locality.hpp>
#include <hpx/runtime/threads/thread_helpers.hpp>
#include <hpx/runtime/parcelset/detail/call_for_each.hpp>
#include <hpx/runtime/parcelset/portals/parcelport.hpp>
#include <hpx/util/runtime_configuration.hpp>
#include <hpx/util/io_service_pool.hpp>
#include <hpx/util/stringstream.hpp>
#include <hpx/util/logging.hpp>

#include <boost/version.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace parcelset { namespace portals
{
    ///////////////////////////////////////////////////////////////////////////
    parcelport::parcelport(util::runtime_configuration const& ini,
            HPX_STD_FUNCTION<void(std::size_t, char const*)> const& on_start_thread,
            HPX_STD_FUNCTION<void()> const& on_stop_thread)
      : parcelset::parcelport(ini),
        io_service_pool_(ini.get_thread_pool_size("parcel_pool"),
	    on_start_thread, on_stop_thread, "parcel_pool_portals", "-portals"),
	    transport_(NULL)
    {
        if (here_.get_type() != connection_portals) {
            HPX_THROW_EXCEPTION(network_error, "portals::parcelport::parcelport",
                "this parcelport was instantiated to represent a unexpected "
                "locality type: " + get_connection_type_name(here_.get_type()));
        }
    }

    parcelport::~parcelport()
    {
      if (NULL != transport_) {
	boost::system::error_code ec;
	transport_->finalize(ec);
	delete transport_;
      }
    }

    util::io_service_pool* parcelport::get_thread_pool(char const* name)
    {
        if (0 == std::strcmp(name, io_service_pool_.get_name()))
            return &io_service_pool_;
        return 0;
    }

    bool parcelport::run(bool blocking)
    {
        io_service_pool_.run(false);    // start pool

        if (NULL == transport_)
	    transport_ = new parcelset::portals::parcelport_transport(*this, parcels_sent_);

        boost::system::error_code ec;
	int ret = transport_->initialize(ec);
        if (ret) {
          HPX_THROW_EXCEPTION(network_error, "portals::parcelport::parcelport",
              "error initializing this parcelport.");
        }

        //for (std::size_t i = 0; i < io_service_pool_.size(); ++i)
        //{
	  io_service_pool_.get_io_service(0).post(
              HPX_STD_BIND(&parcelport_transport::progress, transport_));
        //}

        if (blocking)
            io_service_pool_.join();

        return true;
    }

    void parcelport::stop(bool blocking)
    {
        // make sure no more work is pending, wait for service pool to get empty
        io_service_pool_.stop();

        if (blocking) {
            //io_service_pool_.join();

            if (NULL != transport_)
            {
                boost::system::error_code ec;
                transport_->finalize(ec);
                delete transport_;
                transport_ = NULL;
            }

            io_service_pool_.clear();
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    void parcelport::put_parcel(parcel const& p, write_handler_type f)
    {
        typedef pending_parcels_map::iterator iterator;
        typedef pending_parcels_map::mapped_type mapped_type;

        naming::locality locality_id = p.get_destination_locality();
        naming::gid_type parcel_id = p.get_parcel_id();

        // enqueue the incoming parcel ...
        {
            lcos::local::spinlock::scoped_lock l(mtx_);

            mapped_type& e = pending_parcels_[locality_id];
            e.first.push_back(p);
            e.second.push_back(f);
        }

        error_code ec;
        if (NULL == transport_)
        {
            // If there was an error, we might be safe if there are no parcels
            // to be sent anymore (some other thread already picked them up)
            // or if there are parcels, but the parcel we were about to sent
            // has been already processed.
            lcos::local::spinlock::scoped_lock l(mtx_);

            iterator it = pending_parcels_.find(locality_id);
            if (it != pending_parcels_.end())
            {
                map_second_type const& data = it->second;
                BOOST_FOREACH(parcel const& pp, data.first)
                {
                    if (pp.get_parcel_id() == parcel_id)
                    {
                        // our parcel is still here, bailing out
                        throw hpx::detail::access_exception(ec);
                    }
                }
            }
            return;
        }

        std::vector<parcel> parcels;
        std::vector<write_handler_type> handlers;

        {
            lcos::local::spinlock::scoped_lock l(mtx_);
            iterator it = pending_parcels_.find(locality_id);

            if (it != pending_parcels_.end())
            {
                BOOST_ASSERT(it->first == locality_id);
                std::swap(parcels, it->second.first);
                std::swap(handlers, it->second.second);
            }
        }

        // If the parcels didn't get sent by another connection ...
        if (!parcels.empty() && !handlers.empty())
        {
            send_pending_parcels(parcels, handlers);
        }
    }

    void parcelport::send_pending_parcels_trampoline(
        boost::system::error_code const& ec,
        naming::locality const& locality_id)
    {
        std::vector<parcel> parcels;
        std::vector<write_handler_type> handlers;

        typedef pending_parcels_map::iterator iterator;

        lcos::local::spinlock::scoped_lock l(mtx_);
        iterator it = pending_parcels_.find(locality_id);

        if (it != pending_parcels_.end())
        {
            std::swap(parcels, it->second.first);
            std::swap(handlers, it->second.second);
        }

        if (!parcels.empty() && !handlers.empty())
        {
            // Create a new thread which sends parcels that might still be
            // pending.
            hpx::applier::register_thread_nullary(
                HPX_STD_BIND(&parcelport::send_pending_parcels, this,
                    boost::move(parcels), boost::move(handlers)), "send_pending_parcels");
        }
    }

    void parcelport::send_pending_parcels(
        std::vector<parcel> const & parcels,
        std::vector<write_handler_type> const & handlers)
    {
        std::vector<naming::gid_type> const& gids = parcels[0].get_destinations();
        boost::uint32_t rank = naming::get_locality_id_from_gid(gids[0]);
        naming::locality const& l = parcels[0].get_destination_locality();

        BOOST_ASSERT(transport_ != NULL);
        // store parcels in connection
        // The parcel gets serialized inside set_parcel, no
        // need to keep the original parcel alive after this call returned.
        transport_->set_parcel(parcels);
        transport_->write(rank, l,
            hpx::parcelset::detail::call_for_each(handlers),
            boost::bind(&parcelport::send_pending_parcels_trampoline, this, ::_1, ::_2));
    }

    void early_write_handler(boost::system::error_code const& ec,
        std::size_t size)
    {
        if (ec) {
            // all errors during early parcel handling are fatal
            try {
                HPX_THROW_EXCEPTION(network_error, "early_write_handler",
                    "error while handling early parcel: " +
                        ec.message() + "(" +
                        boost::lexical_cast<std::string>(ec.value())+ ")");
            }
            catch (hpx::exception const& e) {
                hpx::detail::report_exception_and_terminate(e);
            }
            return;
        }
    }

    void early_pending_parcel_handler(boost::system::error_code const& ec,
        naming::locality const&)
    {
        if (ec) {
            // all errors during early parcel handling are fatal
            try {
                HPX_THROW_EXCEPTION(network_error, "early_write_handler",
                    "error while handling early parcel: " +
                        ec.message() + "(" +
                        boost::lexical_cast<std::string>(ec.value())+ ")");
            }
            catch (hpx::exception const& e) {
                hpx::detail::report_exception_and_terminate(e);
            }
            return;
        }
    }

    void parcelport::send_early_parcel(parcel& p)
    {
        std::vector<naming::gid_type> const& gids = p.get_destinations();
        boost::uint32_t rank = naming::get_locality_id_from_gid(gids[0]);
        naming::locality const& l = p.get_destination_locality();
        BOOST_ASSERT(transport_);
        transport_->set_parcel(p);
        transport_->write(rank, l, early_write_handler, early_pending_parcel_handler);
    }

    ///////////////////////////////////////////////////////////////////////////
    void decode_message(parcelport& pp,
        boost::shared_ptr<std::vector<char> > parcel_data,
        boost::uint64_t inbound_data_size,
        performance_counters::parcels::data_point receive_data)
    {
        // protect from un-handled exceptions bubbling up
        try {
            try {
                // mark start of serialization
                util::high_resolution_timer timer;
                boost::int64_t overall_add_parcel_time = 0;

                {
                    // De-serialize the parcel data
                    util::portable_binary_iarchive archive(*parcel_data,
                        inbound_data_size, boost::archive::no_header);

                    std::size_t parcel_count = 0;

                    archive >> parcel_count;
                    for(std::size_t i = 0; i < parcel_count; ++i)
                    {
                        // de-serialize parcel and add it to incoming parcel queue
                        parcel p;
                        archive >> p;

                        // make sure this parcel ended up on the right locality
                        BOOST_ASSERT(p.get_destination_locality() == pp.here());

                        // be sure not to measure add_parcel as serialization time
                        boost::int64_t add_parcel_time = timer.elapsed_nanoseconds();
                        pp.add_received_parcel(p);
                        overall_add_parcel_time += timer.elapsed_nanoseconds() -
                            add_parcel_time;
                    }

                    // complete received data with parcel count
                    receive_data.num_parcels_ = parcel_count;
                    receive_data.raw_bytes_ = archive.bytes_read();
                }

                // store the time required for serialization
                receive_data.serialization_time_ = timer.elapsed_nanoseconds() -
                    overall_add_parcel_time;

                pp.add_received_data(receive_data);
            }
            catch (hpx::exception const& e) {
                LPT_(error)
                    << "decode_message: caught hpx::exception: "
                    << e.what();
                hpx::report_error(boost::current_exception());
            }
            catch (boost::system::system_error const& e) {
                LPT_(error)
                    << "decode_message: caught boost::system::error: "
                    << e.what();
                hpx::report_error(boost::current_exception());
            }
            catch (boost::exception const&) {
                LPT_(error)
                    << "decode_message: caught boost::exception.";
                hpx::report_error(boost::current_exception());
            }
            catch (std::exception const& e) {
                // We have to repackage all exceptions thrown by the
                // serialization library as otherwise we will loose the
                // e.what() description of the problem, due to slicing.
                boost::throw_exception(boost::enable_error_info(
                    hpx::exception(serialization_error, e.what())));
            }
        }
        catch (...) {
            LPT_(error)
                << "decode_message: caught unknown exception.";
            hpx::report_error(boost::current_exception());
        }
    }
}}}
