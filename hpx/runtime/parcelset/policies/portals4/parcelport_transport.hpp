// Copyright (c) 2013 The Trustees of Indiana University and Indiana
//                    University Research and Technology
//                    Corporation.  All rights reserved.
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_PARCELSET_PORTALS4_PARCELPORT_TRANSPORT_HPP)
#define HPX_PARCELSET_PORTALS4_PARCELPORT_TRANSPORT_HPP

#include <sstream>
#include <vector>

#include <hpx/hpx_fwd.hpp>
#include <hpx/util/io_service_pool.hpp>
#include <hpx/runtime/applier/applier.hpp>
#include <hpx/performance_counters/parcels/data_point.hpp>
#include <hpx/performance_counters/parcels/gatherer.hpp>
#include <hpx/util/high_resolution_timer.hpp>

#include <boost/atomic.hpp>
#include <boost/bind.hpp>
#include <boost/bind/protect.hpp>
#include <boost/cstdint.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/integer/endian.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/make_shared.hpp>
#include <boost/static_assert.hpp>
#include <boost/system/system_error.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/scope_exit.hpp>
#include <boost/atomic.hpp>

extern "C" {
#include <portals4.h>
}

///////////////////////////////////////////////////////////////////////////////
// Portals4 Message Tags
#define HPX_FINALIZE_MSG   0x0
#define HPX_PARCEL_MSG     0x1

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace parcelset { namespace portals4
{
    // forward declaration only
    class parcelport;

    void decode_message(parcelport&,
        boost::shared_ptr<std::vector<char> > buffer,
        boost::uint64_t inbound_data_size,
        performance_counters::parcels::data_point receive_data);

    struct recv_block_t {
      void *start;
      ptl_handle_me_t me_h;
    };

    /// This class wraps the Portals 4 API and provides all the required
    /// functionality to communicate using the Portals 4 library.
    class parcelport_transport
      : public boost::enable_shared_from_this<parcelport_transport>,
        private boost::noncopyable
    {
    public:
        parcelport_transport(parcelport& parcelport,
                performance_counters::parcels::gatherer& parcels_sent);

        ~parcelport_transport();

        void set_parcel (parcel const& p)
        {
          set_parcel(std::vector<parcel>(1, p));
        }

        void set_parcel (std::vector<parcel> const& p);

        /// Asynchronously write a data structure to the socket.
        template <typename Handler, typename ParcelPostprocess>
        void write(boost::uint32_t rank, naming::locality const& l,
            Handler handler, ParcelPostprocess parcel_postprocess)
        {
            /// Increment sends and begin timer.
            send_data_.time_ = timer_.elapsed_nanoseconds();

            std::vector<boost::asio::const_buffer> buffers;
            buffers.push_back(boost::asio::buffer(&out_priority_, sizeof(out_priority_)));
            buffers.push_back(boost::asio::buffer(&out_data_size_, sizeof(out_data_size_)));
            buffers.push_back(boost::asio::buffer(&out_size_, sizeof(out_size_)));
            buffers.push_back(boost::asio::buffer(out_buffer_));

            std::size_t psize = boost::asio::buffer_size(buffers);
            std::vector<char> data(psize);
            boost::asio::buffer_copy(boost::asio::buffer(data), buffers);

            // send to the locality
            boost::system::error_code ec;
            parcelport_transport::send(rank, data.data(), data.size());

            // just call initial handler
            handler(ec, psize);

            //parcel_postprocess(ec, l);
        }

        int initialize(boost::system::error_code const& ec);
        void finalize(boost::system::error_code const &ec);
        int send(int dest, void *start, size_t len);
        void progress();

    protected:
        int get_locality_mapping(ptl_handle_ni_t const& ni_h,
            std::size_t num_localities, ptl_process_t* map);
        void repost_recv_block(struct recv_block_t *block);
        void exit_handler(int src, void *start, size_t len);
        void read_handler(int src, void *start, size_t len);
        void read_ack_handler(int src, void *start, size_t len);

    private:
        /// The handler used to process the incoming request.
        parcelset::portals4::parcelport& parcelport_;

        /// buffer for incoming data
        boost::integer::ulittle64_t in_priority_;
        boost::integer::ulittle64_t in_size_;
        boost::integer::ulittle64_t in_data_size_;
        boost::shared_ptr<std::vector<char> > in_buffer_;

        /// buffer for outgoing data
        boost::integer::ulittle64_t out_priority_;
        boost::integer::ulittle64_t out_size_;
        boost::integer::ulittle64_t out_data_size_;
        std::vector<char> out_buffer_;

        /// Counters and their data containers.
        util::high_resolution_timer timer_;
        performance_counters::parcels::data_point send_data_;
        performance_counters::parcels::data_point receive_data_;
        performance_counters::parcels::gatherer& parcels_sent_;

        // archive flags
        int archive_flags_;
    };
}}}

#endif
