// macros.hpp

// Boost Logging library
//
// Author: John Torjo, www.torjo.com
//
// Copyright (C) 2007 John Torjo (see www.torjo.com for email)
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.
// See http://www.torjo.com/log2/ for more details

// IMPORTANT : the JT28092007_macros_HPP_DEFINED needs to remain constant - don't change the macro name!
#ifndef JT28092007_macros_HPP_DEFINED
#define JT28092007_macros_HPP_DEFINED

/* 
    VERY IMPORTANT: 
    Not using #pragma once
    We might need to re-include this file, when defining the logs
*/

#include <boost/logging/detail/fwd.hpp>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <boost/logging/detail/log_keeper.hpp>

namespace boost { namespace logging {

#ifdef BOOST_LOG_COMPILE_FAST_ON
#define BOOST_LOG_COMPILE_FAST
#elif defined(BOOST_LOG_COMPILE_FAST_OFF)
#undef BOOST_LOG_COMPILE_FAST
#else
// by default, turned on
#define BOOST_LOG_COMPILE_FAST
#endif










//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Defining filter Macros 

/*
    when compile fast is "off", we always need BOOST_LOG_MANIPULATE_LOGS, to get access to the logger class typedefs;
*/
#if !defined(BOOST_LOG_COMPILE_FAST) 
#if !defined(BOOST_LOG_MANIPULATE_LOGS)

#define BOOST_LOG_MANIPULATE_LOGS

#endif
#endif







#ifdef BOOST_LOG_COMPILE_FAST
// ****** Fast compile ******

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// use log

#define BOOST_USE_LOG_FIND_GATHER(name,log_type,base_type) \
    namespace boost_log_define_ { \
    extern ::boost::logging::detail::log_keeper< base_type, name ## _boost_log_impl_, ::boost::logging::detail::call_write_logger_finder< log_type, gather_msg > ::type > name ; \
    } \
    using boost_log_define_:: name ;


#define BOOST_USE_LOG(name,type) BOOST_USE_LOG_FIND_GATHER(name, type, ::boost::logging::detail::fast_compile_with_default_gather<>::log_type )

#define BOOST_DECLARE_LOG_KEEPER(name,log_type) \
    namespace boost_log_declare_ { \
    extern ::boost::logging::detail::log_keeper< log_type, name ## _boost_log_impl_ > name; \
    } \
    using boost_log_declare_ :: name ;




#if !defined(BOOST_LOG_MANIPULATE_LOGS)
// user is declaring logs
#define BOOST_DECLARE_LOG_WITH_LOG_TYPE(name,log_type) \
    log_type& name ## _boost_log_impl_(); \
    BOOST_DECLARE_LOG_KEEPER(name,log_type)

#else
// user is defining logs
#define BOOST_DECLARE_LOG_WITH_LOG_TYPE(name,log_type) \
    log_type& name ## _boost_log_impl_(); \
    BOOST_USE_LOG(name,log_type)

#endif


#define BOOST_DECLARE_LOG_FIND_GATHER(name) \
    BOOST_DECLARE_LOG_WITH_LOG_TYPE(name, ::boost::logging::detail::fast_compile_with_default_gather<>::log_type )

#define BOOST_DECLARE_LOG(name,type) BOOST_DECLARE_LOG_FIND_GATHER(name)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// define log
#define BOOST_DEFINE_LOG_FIND_GATHER(name, log_type, base_type, gather_msg)  base_type & name ## _boost_log_impl_() \
{ typedef ::boost::logging::detail::call_write_logger_finder< log_type, gather_msg > ::type logger_type; \
  static logger_type i; return i; } \
    namespace { boost::logging::detail::fake_using_log ensure_log_is_created_before_main ## name ( name ## _boost_log_impl_() ); } \
    namespace boost_log_declare_ { \
    ::boost::logging::detail::log_keeper< base_type, name ## _boost_log_impl_ > name ; \
    } \
    namespace boost_log_define_ { \
    ::boost::logging::detail::log_keeper< base_type, name ## _boost_log_impl_, ::boost::logging::detail::call_write_logger_finder< log_type, gather_msg > ::type > name ; \
    } \
    using boost_log_define_ :: name ; 

#define BOOST_DEFINE_LOG(name,type) \
    BOOST_DEFINE_LOG_FIND_GATHER(name, type, ::boost::logging::detail::fast_compile_with_default_gather<>::log_type, ::boost::logging::detail::fast_compile_with_default_gather<>::gather_msg )







#else
// don't compile fast

#define BOOST_DECLARE_LOG(name,type) type& name ## _boost_log_impl_(); extern boost::logging::detail::log_keeper<type, name ## _boost_log_impl_ > name; 
#define BOOST_DEFINE_LOG(name,type)  type& name ## _boost_log_impl_() \
    { static type i; return i; } \
    namespace { boost::logging::detail::fake_using_log ensure_log_is_created_before_main ## name ( name ## _boost_log_impl_() ); } \
    boost::logging::detail::log_keeper<type, name ## _boost_log_impl_ > name; 

/** 
    Advanced
*/
#define BOOST_DECLARE_LOG_WITH_GATHER(name,type,gather_type) BOOST_DECLARE_LOG(name,type)

#endif





//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Filter Macros 

#define BOOST_DECLARE_LOG_FILTER_NO_NAMESPACE_PREFIX(name,type) type& name ## _boost_log_filter_impl_(); extern boost::logging::detail::log_filter_keeper<type, name ## _boost_log_filter_impl_ > name; 
#define BOOST_DEFINE_LOG_FILTER_NO_NAMESPACE_PREFIX(name,type)  type& name ## _boost_log_filter_impl_() \
    { static type i; return i; } \
    namespace { boost::logging::detail::fake_using_log ensure_log_is_created_before_main ## name ( name ## _boost_log_filter_impl_() ); } \
    boost::logging::detail::log_filter_keeper<type, name ## _boost_log_filter_impl_ > name; 


#define BOOST_DECLARE_LOG_FILTER(name,type) BOOST_DECLARE_LOG_FILTER_NO_NAMESPACE_PREFIX(name, ::boost::logging:: type)

#define BOOST_DEFINE_LOG_FILTER(name,type) BOOST_DEFINE_LOG_FILTER_NO_NAMESPACE_PREFIX(name, ::boost::logging:: type)








//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Log Macros



#define BOOST_LOG_USE_LOG(l, do_func, is_log_enabled) if ( !(is_log_enabled) ) ; else l -> do_func

#define BOOST_LOG_USE_LOG_IF_LEVEL(l, holder, the_level) BOOST_LOG_USE_LOG(l, read_msg().gather().out(), holder->is_enabled(::boost::logging::level:: the_level) )

#define BOOST_LOG_USE_LOG_IF_FILTER(l, the_filter) BOOST_LOG_USE_LOG(l, read_msg().gather().out(), the_filter)

#define BOOST_LOG_USE_SIMPLE_LOG_IF_FILTER(l, is_log_enabled) if ( !(is_log_enabled) ) ; else l ->operator() 





//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Format and Destination Macros

#define BOOST_LOG_FORMAT_MSG(msg_class) \
    namespace boost { namespace logging { \
    template<> struct formatter::msg_type<override> { typedef msg_class & type; }; \
    }}

#define BOOST_LOG_DESTINATION_MSG(msg_class) \
    namespace boost { namespace logging { \
    template<> struct destination::msg_type<override> { typedef const msg_class & type; }; \
    }}



}}

#endif

