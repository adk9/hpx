//  Copyright (c) 2011 Matthew Anderson
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_68F602E3_C235_4660_AEAC_D5BD7AEC4805)
#define HPX_68F602E3_C235_4660_AEAC_D5BD7AEC4805

#include <hpx/runtime/components/client_base.hpp>

#include "stubs/point.hpp"

namespace gtc
{
    ///////////////////////////////////////////////////////////////////////////
    /// The client side representation of a \a gtc::server::point components.
    class point : public hpx::components::client_base<point, stubs::point>
    {
        typedef hpx::components::client_base<point, stubs::point>
            base_type;

    public:
        /// Default construct an empty client side representation (not
        /// connected to any existing component).
        point()
        {}

        /// Create a client side representation for the existing
        /// \a gtc::server::point instance with the given GID.
        point(hpx::naming::id_type const& gid)
          : base_type(gid)
        {}

        ///////////////////////////////////////////////////////////////////////
        // Exposed functionality of this component.

        hpx::lcos::future<void> setup_async(std::size_t numberpe,std::size_t mype)
        {
            BOOST_ASSERT(gid_);
            return this->base_type::setup_async(gid_,numberpe,mype);
        }

        void setup(std::size_t numberpe,std::size_t mype)
        {
            BOOST_ASSERT(gid_);
            this->base_type::setup(gid_,numberpe,mype);
        }
    };
}

#endif
