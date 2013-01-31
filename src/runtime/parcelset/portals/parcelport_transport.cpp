// Copyright (c) 2013 The Trustees of Indiana University and Indiana
//                    University Research and Technology
//                    Corporation.  All rights reserved.
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx.hpp>
#include <hpx/hpx_fwd.hpp>
#include <hpx/util/io_service_pool.hpp>
#include <hpx/runtime/parcelset/portals/parcelport_transport.hpp>
#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/util/stringstream.hpp>
#include <hpx/traits/type_size.hpp>
#include <hpx/runtime/agas/interface.hpp>
#include <hpx/runtime/agas/interface.hpp>

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/static_assert.hpp>
#include <boost/system/system_error.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/scope_exit.hpp>
#include <boost/atomic.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/archive/basic_binary_oarchive.hpp>
#include <boost/format.hpp>

extern "C" {
#include <portals4.h>
}

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace parcelset { namespace portals
{
    ptl_handle_ni_t  ni_h;
    ptl_handle_eq_t  eq_h;
    ptl_handle_md_t  md_h;
    recv_block_t    *recv_blocks     = NULL;
    ptl_handle_me_t  overflow_me_h;
    ptl_pt_index_t   data_pt;
    ptl_ni_limits_t  ni_limits, ni_req_limits;
    ptl_md_t         md;
    ptl_me_t         me;
    size_t           num_recv_blocks = 3;
    size_t           size_recv_block = 1024*1024;

    parcelport_transport::parcelport_transport(parcelport& parcelport,
            performance_counters::parcels::gatherer& parcels_sent)
        : parcelport_(parcelport), in_priority_(0), in_size_(0), in_data_size_(0),
          in_buffer_(), out_priority_(0), out_size_(0), out_data_size_(0),
          out_buffer_(), parcels_sent_(parcels_sent),
	  archive_flags_(boost::archive::no_header)
    {
#ifdef BOOST_BIG_ENDIAN
        std::string endian_out = get_config_entry("hpx.parcel.endian_out", "big");
#else
        std::string endian_out = get_config_entry("hpx.parcel.endian_out", "little");
#endif
        if (endian_out == "little")
            archive_flags_ |= util::endian_little;
        else if (endian_out == "big")
            archive_flags_ |= util::endian_big;
        else {
            BOOST_ASSERT(endian_out =="little" || endian_out == "big");
        }
        in_buffer_.reset(new std::vector<char>());
    }

    parcelport_transport::~parcelport_transport()
    {
    }

    int parcelport_transport::get_locality_mapping(ptl_handle_ni_t const& ni_h,
        std::size_t num_localities, ptl_process_t *map)
    {
      boost::system::error_code ec;

      runtime& rt = get_runtime();
      naming::locality loc = rt.here();
      //std::vector<naming::locality> localities = rt.get_agas_client().get_resolved_localities();
      std::vector<naming::locality> localities;
      localities.push_back(loc);
      localities.push_back(loc);

      BOOST_ASSERT(num_localities == localities.size());

      //std::vector<naming::gid_type> locality_ids = agas_client.get_localities(locality_ids);
      //BOOST_ASSERT(num_localities == locality_ids.size());

      for (size_t i = 0; i < num_localities; i++) {
          boost::asio::ip::address addr;
          addr = boost::asio::ip::address::from_string(localities[i].get_address(), ec);
          if (ec) {
              HPX_THROW_EXCEPTION(network_error, "portals::parcelport::parcelport_transport",
                  "error resolving locality: " + ec.message());
          }
          //map[i].phys.nid = addr.to_v4().to_ulong();
          //map[i].phys.pid = localities[i].get_port();
          map[i].phys.nid = 3232236035+i;
          map[i].phys.pid = 7910+i;
          //fprintf(stderr, "Got {%u, %u}, Wanted {%lu, %lu}\n",
          //        (unsigned int)map[i].phys.nid,
          //        (unsigned int)map[i].phys.pid,
          //        3232236035+i, 7910+i);
      }

      return PTL_OK;
    }

    int parcelport_transport::initialize(boost::system::error_code const& ec)
    {
        LTM_(debug) << "parcelport_transport::initialize() ";
	int ret = PtlInit();
	BOOST_ASSERT(ret == PTL_OK);

	ni_req_limits.max_entries            = 1024;
	ni_req_limits.max_unexpected_headers = 1024;
	ni_req_limits.max_mds                = 1024;
	ni_req_limits.max_eqs                = 1024;
	ni_req_limits.max_cts                = 1024;
	ni_req_limits.max_pt_index           = 64; 
	ni_req_limits.max_iovecs             = 1024;
	ni_req_limits.max_list_size          = 1024;
	ni_req_limits.max_triggered_ops      = 1024;
	ni_req_limits.max_msg_size           = LONG_MAX;
	ni_req_limits.max_atomic_size        = 512;
	ni_req_limits.max_fetch_atomic_size  = 512;
	ni_req_limits.max_waw_ordered_size   = 512;
	ni_req_limits.max_war_ordered_size   = 512;
	ni_req_limits.max_volatile_size      = 512;
	ni_req_limits.features               = 0;

        runtime& rt = get_runtime();
        util::runtime_configuration const& cfg = rt.get_config();
        std::size_t const num_localities = cfg.get_num_localities();
        naming::locality loc = rt.here();

        ptl_process_t* desired = (ptl_process_t*) malloc(sizeof(ptl_process_t) * num_localities);
        BOOST_ASSERT(desired != NULL);
	ret = get_locality_mapping(ni_h, num_localities, desired);
        BOOST_ASSERT(ret == PTL_OK);

	ret = PtlNIInit(PTL_IFACE_DEFAULT, PTL_NI_MATCHING | PTL_NI_LOGICAL,
			loc.get_port(), &ni_req_limits, &ni_limits, &ni_h);
	BOOST_ASSERT(ret == PTL_OK);

        ptl_process_t my_id, lid;
        ret = PtlGetPhysId(ni_h, &my_id);
        if (ret != PTL_OK) {
	    fprintf(stderr, "PtlGetPhysId: %d\n", ret);
	    return ret;
	}
        //fprintf(stderr, "==> {%u, %u}\n", my_id.phys.pid, my_id.phys.nid);

        /*
        boost::uint32_t my_locality = naming::get_locality_id_from_gid(
            get_runtime().get_agas_client().local_locality());

        // sanity checks
        lid = desired[my_locality];
        BOOST_ASSERT(my_id.phys.pid == lid.phys.pid);
        BOOST_ASSERT(my_id.phys.nid == lid.phys.nid);
        */

	ret = PtlSetMap(ni_h, num_localities, desired);
	if ((PTL_OK != ret) && (PTL_IGNORED != ret)) {
	    fprintf(stderr, "PtlSetMap: %d\n", ret);
	    return ret;
	}

	/* create completion EQ/CT and portal table entry */
	ret = PtlEQAlloc(ni_h, 1 * 1024 * 1024, &eq_h);
	if (PTL_OK != ret) {
	    fprintf(stderr, "PtlEQAlloc: %d\n", ret);
	    return ret;
	}

	ret = PtlPTAlloc(ni_h, 0, eq_h, 19, &data_pt);
	if (PTL_OK != ret) {
	    fprintf(stderr, "PtlPTAlloc: %d\n", ret);
	    return ret;
	}

	/* truncating mes */
	me.start     = NULL;
	me.length    = 0;
	me.ct_handle = PTL_CT_NONE;
	me.min_free  = 0;
	me.uid       = PTL_UID_ANY;
	me.options   = PTL_ME_OP_PUT | PTL_ME_EVENT_SUCCESS_DISABLE
           | PTL_ME_UNEXPECTED_HDR_DISABLE;
	me.match_id.rank = PTL_RANK_ANY;
	me.match_bits    = 0x0;
	me.ignore_bits   = ~(0x0ULL);
	ret              = PtlMEAppend(ni_h, 19, &me, PTL_OVERFLOW_LIST, NULL,
				       &overflow_me_h);
	if (PTL_OK != ret) {
	    fprintf(stderr, "PtlMEAppend: %d\n", ret);
	    return ret;
	}

	/* post receive mes */
	recv_blocks = (recv_block_t*)malloc(sizeof(struct recv_block_t) * num_recv_blocks);
	if (NULL == recv_blocks) { return -1; }
	for (size_t i = 0; i < num_recv_blocks; ++i) {
	    recv_blocks[i].start = malloc(size_recv_block);
	    if (NULL == recv_blocks[i].start) { return -1; }
	    recv_blocks[i].me_h = PTL_INVALID_HANDLE;
	    parcelport_transport::repost_recv_block(&recv_blocks[i]);
	}

	md.start     = 0;
	md.length    = SIZE_MAX;
        md.options   = PTL_MD_UNORDERED;
	md.eq_handle = eq_h;
        
        ret = PtlCTAlloc(ni_h, &md.ct_handle);
        if (PTL_OK != ret) {
            fprintf(stderr, "PtlCTAlloc: %d\n", ret);
            return ret;
        }
        
	ret = PtlMDBind(ni_h, &md, &md_h);
	if (PTL_OK != ret) {
	    fprintf(stderr, "PtlMDBind: %d\n", ret);
	    return ret;
	}

	return ret;
    }

    void parcelport_transport::finalize(boost::system::error_code const& ec)
    {
      int ret;
      for (size_t i = 0; i < num_recv_blocks; ++i) {
        ret = PtlMEUnlink(recv_blocks[i].me_h);
        if (PTL_OK != ret) {
            std::cerr << "PtlMEUnlink returned " << ret << "\n";
        }
      }

      ret = PtlMEUnlink(overflow_me_h);
      if (PTL_OK != ret) {
          std::cerr << "PtlMEUnlink returned " << ret << "\n";
      }

      ret = PtlMDRelease(md_h);
      if (PTL_OK != ret) {
          std::cerr << "PtlMDRelease returned " << ret << "\n";
      }

      ret = PtlPTFree(ni_h, data_pt);
      if (PTL_OK != ret) {
          std::cerr << "PtlPTFree returned " << ret << "\n";
      }

      ret = PtlEQFree(eq_h);
      if (PTL_OK != ret) {
          std::cerr << "PtlEQFree returned " << ret << "\n";
      }
      
      PtlNIFini(ni_h);
      PtlFini();
    }

    void parcelport_transport::repost_recv_block(struct recv_block_t *block)
    {
      ptl_me_t me;

      me.start     = block->start;
      me.length    = size_recv_block;
      me.ct_handle = PTL_CT_NONE;
      me.min_free  = 4096;
      me.uid       = PTL_UID_ANY;
      me.options   = PTL_ME_OP_PUT |
              PTL_ME_MANAGE_LOCAL |
              PTL_ME_NO_TRUNCATE |
              PTL_ME_MAY_ALIGN;
      me.match_id.rank = PTL_RANK_ANY;
      me.match_bits    = 0;
      me.ignore_bits   = ~(0ULL);

      int ret = PtlMEAppend(ni_h, 19, &me, PTL_PRIORITY_LIST, block, &block->me_h);
      if (PTL_OK != ret) {
          HPX_THROW_EXCEPTION(network_error, "portals::parcelport::parcelport_transport",
              "error reposting recv block.");
      }
    }

    int parcelport_transport::send(int dest, void *start, size_t len)
    {
        LTM_(debug) << "portals::send() " << dest;
	ptl_process_t peer;

        // boost::integer::ulittle64_t out;
        // out = *((boost::integer::ulittle64_t *)(((uint8_t *)start)));
        // std::cerr << "out_priority_=" << out << std::endl;
        // out = *((boost::integer::ulittle64_t *)(((uint8_t *)start)+sizeof(out_priority_)));
        // std::cerr << "out_data_size_=" << out << std::endl;
        // out = *((boost::integer::ulittle64_t *)(((uint8_t *)start)+sizeof(out_priority_)+sizeof(out_size_)));
        // std::cerr << "out_size_=" << out << std::endl;

        peer.rank = dest;
	int ret = PtlPut(md_h, (ptl_size_t)start, len, PTL_ACK_REQ, peer,
                         19, 0, 0, start, HPX_PARCEL_MSG);
	if (PTL_OK != ret) {
            std::cerr << "PtlPut failed: " << ret << "\n";
	}

	return ret;
    }

    void parcelport_transport::read_handler(int src, void *start, size_t len)
    {
        std::vector<char> payload(len);
        std::memcpy(payload.data(), start, len);

        in_buffer_->clear();
        in_priority_ = 0;
        in_size_ = 0;
        in_data_size_ = 0;

        std::vector<boost::asio::mutable_buffer> buffers;
        buffers.push_back(boost::asio::buffer(&in_priority_, sizeof(in_priority_)));
        buffers.push_back(boost::asio::buffer(&in_data_size_, sizeof(in_data_size_)));
        buffers.push_back(boost::asio::buffer(&in_size_, sizeof(in_size_)));

        std::size_t bsize = boost::asio::buffer_size(buffers);
        boost::asio::buffer_copy(buffers, boost::asio::buffer(payload, bsize));

        // std::cerr << "in_priority=" << in_priority_ << std::endl;
        //std::cerr << "in_size=" << in_size_ << std::endl;
        //std::cerr << "in_data_size=" << in_data_size_ << std::endl;
        
        boost::uint64_t inbound_size = in_data_size_;
        in_buffer_->resize(static_cast<std::size_t>(inbound_size));
        std::memcpy(in_buffer_->data(), &payload[0]+bsize, len-bsize);
        //std::copy(payload.begin()+bsize, payload.end(), in_buffer_->begin());

        // std::copy(in_buffer_->begin(), in_buffer_->end(), std::ostream_iterator<char>(std::cerr, ""));
        // std::cerr << std::endl;
        
        receive_data_.bytes_ = std::size_t(inbound_size);
        receive_data_.time_ = timer_.elapsed_nanoseconds() -
                receive_data_.time_;
        
        // hold on to received data to avoid data races
        boost::shared_ptr<std::vector<char> > data(in_buffer_);
        performance_counters::parcels::data_point receive_data = receive_data_;

        boost::uint64_t inbound_data_size = in_data_size_;
        receive_data_.bytes_ = std::size_t(inbound_data_size);

        // add parcel data to incoming parcel queue
        decode_message(parcelport_, data, inbound_data_size, receive_data);
    }

    void parcelport_transport::exit_handler(int src, void *start, size_t len)
    {
        LTM_(debug) << "end progress function\n";
    }

    void parcelport_transport::read_ack_handler(int src, void *start, size_t len)
    {
        LTM_(debug) << "ack received\n";
    }

    void parcelport_transport::progress()
    {
        int ret;
        ptl_event_t ev;

        LTM_(debug) << "begin progress function\n";
        while (1) {
            
            ret = PtlEQWait(eq_h, &ev);
            if ((PTL_OK == ret) || (PTL_EQ_DROPPED == ret)) {
                switch (ev.type) {
                case PTL_EVENT_PUT:
                    BOOST_ASSERT(ev.mlength == ev.rlength);
                    /*
                    fprintf(stderr, "ev.start = %p, ev.user_ptr = %p, ev.hdr_data = %lu\n, ev.rlength = %lu, ev.mlength = %lu, ev.remote_offset = %lu, ev.uid = %d, ev.pt_index = %u, ev.ni_fail_type = %d\n",
                            ev.start, ev.user_ptr, ev.hdr_data,
                            ev.rlength, ev.mlength, ev.remote_offset,
                            ev.uid, ev.pt_index, ev.ni_fail_type);
                    */
                    switch(ev.hdr_data) {
                    case HPX_FINALIZE_MSG:
                        parcelport_transport::exit_handler(ev.initiator.rank, ev.start, ev.mlength);
                        return;
                    case HPX_PARCEL_MSG:
                        // Store the time of the begin of the read operation
                        receive_data_.time_ = timer_.elapsed_nanoseconds();
                        receive_data_.serialization_time_ = 0;
                        receive_data_.bytes_ = 0;
                        receive_data_.num_parcels_ = 0;
                        // {
                        // boost::integer::ulittle64_t in;
                        // in = *((boost::integer::ulittle64_t *)(((uint8_t *)ev.start)));
                        // std::cerr << "in_priority_=" << in << std::endl;
                        // in = *((boost::integer::ulittle64_t *)(((uint8_t *)ev.start)+sizeof(in_priority_)));
                        // std::cerr << "in_data_size_=" << in << std::endl;
                        // in = *((boost::integer::ulittle64_t *)(((uint8_t *)ev.start)+sizeof(in_priority_) + sizeof(in_size_)));
                        // std::cerr << "in_size_=" << in << std::endl;
                        // }
                        
                        parcelport_transport::read_handler(ev.initiator.rank, ev.start, ev.mlength);
                        break;
                    default:
                        std::cerr << "Got message with unregistered tag " <<
                                (long unsigned)ev.hdr_data << "(ignoring)\n";
                    }
                case PTL_EVENT_SEND:
                    if (PTL_NI_OK != ev.ni_fail_type) {
                        std::cerr << "SEND event with fail type " << ev.ni_fail_type << "\n";
                    } else {
		      // complete data point and push back onto gatherer
		      send_data_.time_ = timer_.elapsed_nanoseconds() - send_data_.time_;
		      parcels_sent_.add_data(send_data_);

		      out_buffer_.clear();
		      out_priority_ = 0;
		      out_size_ = 0;
		      out_data_size_ = 0;

		      send_data_.bytes_ = 0;
		      send_data_.time_ = 0;
		      send_data_.serialization_time_ = 0;
		      send_data_.num_parcels_ = 0;
		    }
                    break;
                case PTL_EVENT_ACK:
                    if (PTL_NI_OK != ev.ni_fail_type) {
                        std::cerr << "SEND event with fail type " << ev.ni_fail_type << "\n";
                    } else {
                        if (PTL_OK != ret) {
                            std::cerr << "transmit packet failed: " << ret << "\n";
                        }
                        //parcelport_transport::read_ack_handler(ev.initiator.rank, ev.start, ev.mlength);
                    }
                    break;
                case PTL_EVENT_AUTO_UNLINK:
                    {
                        std::cerr << "AUTO_UNLINK\n";
                        struct recv_block_t *block = (struct recv_block_t *)ev.user_ptr;
                        parcelport_transport::repost_recv_block(block);
                        break;
                    }
                default:
                    break;
                }
            }
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    void parcelport_transport::set_parcel(std::vector<parcel> const& pv)
    {
#if defined(HPX_DEBUG)
        // make sure that all parcels go to the same locality
        BOOST_FOREACH(parcel const& p, pv)
        {
            naming::locality const locality_id = p.get_destination_locality();
            BOOST_ASSERT(locality_id == destination());
        }
#endif

        // we choose the highest priority of all parcels for this message
        threads::thread_priority priority = threads::thread_priority_default;

        // collect argument sizes from parcels
        std::size_t arg_size = 0;

        // guard against serialization errors
        try {
            // clear and preallocate out_buffer_
            out_buffer_.clear();

            BOOST_FOREACH(parcel const & p, pv)
            {
                arg_size += traits::get_type_size(p);
                priority = (std::max)(p.get_thread_priority(), priority);
            }

            out_buffer_.reserve(arg_size*2);

            // mark start of serialization
            util::high_resolution_timer timer;

            {
                // Serialize the data
                HPX_STD_UNIQUE_PTR<util::binary_filter> filter(
                    pv[0].get_serialization_filter());

                int archive_flags = archive_flags_;
                if (filter.get() != 0) {
                    filter->set_max_length(out_buffer_.capacity());
                    archive_flags |= util::enable_compression;
                }

                util::portable_binary_oarchive archive(
                    out_buffer_, filter.get(), archive_flags);

                std::size_t count = pv.size();
                archive << count;

                BOOST_FOREACH(parcel const & p, pv)
                {
                    archive << p;
                }

                arg_size = archive.bytes_written();
            }

            // store the time required for serialization
            send_data_.serialization_time_ = timer.elapsed_nanoseconds();
        }
        catch (boost::archive::archive_exception const& e) {
            // We have to repackage all exceptions thrown by the
            // serialization library as otherwise we will loose the
            // e.what() description of the problem.
            HPX_THROW_EXCEPTION(serialization_error,
                "portals::parcelport_transport::set_parcel",
                boost::str(boost::format(
                    "parcelport: parcel serialization failed, caught "
                    "boost::archive::archive_exception: %s") % e.what()));
            return;
        }
        catch (boost::system::system_error const& e) {
            HPX_THROW_EXCEPTION(serialization_error,
                "portals::parcelport_transport::set_parcel",
                boost::str(boost::format(
                    "parcelport: parcel serialization failed, caught "
                    "boost::system::system_error: %d (%s)") %
                        e.code().value() % e.code().message()));
            return;
        }
        catch (std::exception const& e) {
            HPX_THROW_EXCEPTION(serialization_error,
                "portals::parcelport_transport::set_parcel",
                boost::str(boost::format(
                    "parcelport: parcel serialization failed, caught "
                    "std::exception: %s") % e.what()));
            return;
        }

        out_priority_ = boost::integer::ulittle64_t(priority);
        out_size_ = out_buffer_.size();
        out_data_size_ = arg_size;

        send_data_.num_parcels_ = pv.size();
        send_data_.bytes_ = arg_size;
        send_data_.raw_bytes_ = out_buffer_.size();
    }
}}}
