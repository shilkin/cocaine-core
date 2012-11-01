/*
    Copyright (c) 2011-2012 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2012 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>. 
*/

#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>

#include "cocaine/slave/slave.hpp"

#include "cocaine/context.hpp"
#include "cocaine/logging.hpp"
#include "cocaine/manifest.hpp"
#include "cocaine/profile.hpp"
#include "cocaine/rpc.hpp"

#include "cocaine/traits/unique_id.hpp"

using namespace cocaine;
using namespace cocaine::engine;

namespace fs = boost::filesystem;

slave_t::slave_t(context_t& context,
                 slave_config_t config):
    m_context(context),
    m_log(context.log(
        (boost::format("app/%1%")
            % config.name
        ).str()
    )),
    m_id(config.uuid),
    m_name(config.name),
    m_bus(context, ZMQ_DEALER, m_id),
    m_bus_timeout(m_bus, defaults::bus_timeout)
{
    uint64_t hwm = 10;

    // TODO: Make it SND_HWM in ZeroMQ 3.1+
    m_bus.setsockopt(ZMQ_HWM, &hwm, sizeof(hwm));

    std::string endpoint(
        (boost::format("ipc://%1%/%2%")
            % m_context.config.ipc_path
            % m_name
        ).str()
    );
    
    m_bus.connect(endpoint);
    
    m_watcher.set<slave_t, &slave_t::on_event>(this);
    m_watcher.start(m_bus.fd(), ev::READ);
    m_checker.set<slave_t, &slave_t::on_check>(this);
    m_checker.start();

    m_heartbeat_timer.set<slave_t, &slave_t::on_heartbeat>(this);
    m_heartbeat_timer.start(0.0f, 5.0f);

    // NOTE: It will be restarted after the each heartbeat.
    m_disown_timer.set<slave_t, &slave_t::on_disown>(this);

    // Launching the app

    try {
        m_manifest.reset(new manifest_t(m_context, m_name));
        m_profile.reset(new profile_t(m_context, config.profile));
        
        m_idle_timer.set<slave_t, &slave_t::on_idle>(this);
        m_idle_timer.start(m_profile->idle_timeout);
        
        fs::path path(fs::path(m_context.config.spool_path) / m_name);
         
        m_sandbox = m_context.get<api::sandbox_t>(
            m_manifest->sandbox.type,
            api::category_traits<api::sandbox_t>::args_type(
                m_manifest->name,
                m_manifest->sandbox.args,
                path.string()
            )
        );
    } catch(const std::exception& e) {
        io::message<rpc::error> message(server_error, e.what());
        m_bus.send_message(message);
        terminate();
        throw;
    } catch(...) {
        io::message<rpc::error> message(
            server_error,
            "unexpected exception while configuring the slave"
        );
        
        m_bus.send_message(message);
        terminate();
        throw;
    }
}

slave_t::~slave_t() { }

void
slave_t::run() {
    m_loop.loop();
}

std::string
slave_t::read(int timeout) {
    int command = -1;
    zmq::message_t body;

    boost::tuple<
        int&,
        zmq::message_t*
    > proxy(command, &body);
        
    {
        io::scoped_option<
            io::options::receive_timeout,
            io::policies::unique
        > option(m_bus, timeout);

        if(!m_bus.recv_tuple(proxy)) {
            return std::string();
        }
    }

    // TEST: Ensure that we have the correct message type.
    BOOST_ASSERT(command == io::get<rpc::chunk>::value);

    return std::string(
        static_cast<const char*>(body.data()),
        body.size()
    );
}

void
slave_t::write(const void * data,
               size_t size)
{
    zmq::message_t body(size);

    memcpy(body.data(), data, size);
    io::message<rpc::chunk> message(body);

    m_bus.send_message(message);
}

void
slave_t::on_event(ev::io&, int) {
    m_checker.stop();

    if(m_bus.pending()) {
        m_checker.start();
        process_events();
    }
}

void
slave_t::on_check(ev::prepare&, int) {
    m_loop.feed_fd_event(m_bus.fd(), ev::READ);
}

void
slave_t::process_events() {
    // TEST: Ensure that we haven't missed something in a previous iteration.
    BOOST_ASSERT(!m_bus.more());
   
    int command = -1;

    {
        io::scoped_option<
            io::options::receive_timeout,
            io::policies::unique
        > option(m_bus, 0);
        
        if(!m_bus.recv(command)) {
            return;
        }
    }

    COCAINE_LOG_DEBUG(
        m_log,
        "slave %s received type %d message",
        m_id,
        command
    );

    switch(command) {
        case io::get<rpc::pong>::value:
            m_disown_timer.stop();
            break;

        case io::get<rpc::invoke>::value: {
            std::string event;

            m_bus.recv(event);
            
            invoke(event);
            
            break;
        }
        
        case io::get<rpc::chunk>::value:
            // Drop outstanding chunks from the previous job.
            m_bus.drop();
            break;
        
        case io::get<rpc::terminate>::value:
            terminate();
            break;

        default:
            COCAINE_LOG_WARNING(
                m_log,
                "slave %s dropping unknown type %d message", 
                m_id,
                command
            );
            
            m_bus.drop();
    }
}

void
slave_t::on_heartbeat(ev::timer&, int) {
    m_bus.send_message(io::message<rpc::ping>());
    m_disown_timer.start(5.0f);
}

void
slave_t::on_disown(ev::timer&, int) {
    COCAINE_LOG_ERROR(m_log, "slave %s has lost the controlling engine", m_id.string());
    m_loop.unloop();    
}

void
slave_t::on_idle(ev::timer&, int) {
    terminate();
}

void
slave_t::invoke(const std::string& event) {
    // TEST: Ensure that we have the app first.
    BOOST_ASSERT(m_sandbox.get() != NULL);

    try {
        m_sandbox->invoke(event, *this);
    } catch(const unrecoverable_error_t& e) {
        io::message<rpc::error> message(server_error, e.what());
        m_bus.send_message(message);
    } catch(const std::exception& e) {
        io::message<rpc::error> message(app_error, e.what());
        m_bus.send_message(message);
    } catch(...) {
        io::message<rpc::error> message(
            server_error,
            "unexpected exception while processing an event"
        );
        
        m_bus.send_message(message);
    }
    
    m_bus.send_message(io::message<rpc::choke>());
   
    // Rearm the idle timer. 
    m_idle_timer.stop();
    m_idle_timer.start(m_profile->idle_timeout);

    // Feed the event loop.
    m_loop.feed_fd_event(m_bus.fd(), ev::READ);
}

void
slave_t::terminate() {
    m_bus.send_message(io::message<rpc::terminate>());
    m_loop.unloop(ev::ALL);
}
