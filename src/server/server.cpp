/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2024 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "pch.hpp"

#include "server/network/message/outputmessage.hpp"
#include "server/server.hpp"
#include "config/configmanager.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "creatures/players/management/ban.hpp"
#include "lib/di/container.hpp"

ServiceManager &ServiceManager::getInstance() {
	return inject<ServiceManager>();
}

ServiceManager::~ServiceManager() {
	try {
		stop();
	} catch (std::exception &exception) {
		g_logger().error("{} - Catch exception error: {}", __FUNCTION__, exception.what());
	}
}

void ServiceManager::die() {
	work.reset(); // Permite que io_context.run() retorne quando não houver mais trabalho
	for (auto &thread : threads) {
		if (thread.joinable()) {
			thread.join();
		}
	}
	io_context.stop();
}

void ServiceManager::run() {
	if (running) {
		g_logger().error("ServiceManager is already running!", __FUNCTION__);
		return;
	}

	assert(!running);
	running = true;
	io_context.run();
}

void ServiceManager::stop() {
	if (!running) {
		return;
	}

	running = false;

	for (auto &servicePortIt : acceptors) {
		try {
			io_context.post([servicePort = servicePortIt.second] { servicePort->onStopServer(); });
		} catch (const std::system_error &e) {
			g_logger().warn("[ServiceManager::stop] - Network error: {}", e.what());
		}
	}

	acceptors.clear();

	death_timer.expires_from_now(std::chrono::seconds(3));
	death_timer.async_wait([this](const std::error_code &err) {
		die();
	});
}

ServicePort::~ServicePort() {
	close();
}

bool ServicePort::is_single_socket() const {
	return !services.empty() && services.front()->is_single_socket();
}

std::string ServicePort::get_protocol_names() const {
	if (services.empty()) {
		return {};
	}

	std::string str = services.front()->get_protocol_name();
	for (size_t i = 1; i < services.size(); ++i) {
		str.push_back(',');
		str.push_back(' ');
		str.append(services[i]->get_protocol_name());
	}
	return str;
}

void ServicePort::accept() {
	if (!acceptor) {
		return;
	}

	auto connection = ConnectionManager::getInstance().createConnection(io_context, shared_from_this());
	acceptor->async_accept(connection->getSocket(), [self = shared_from_this(), connection](const std::error_code &error) { self->onAccept(connection, error); });
}

void ServicePort::onAccept(const Connection_ptr &connection, const std::error_code &error) {
	if (!error) {
		if (services.empty()) {
			return;
		}

		auto remote_ip = connection->getIP();
		if (remote_ip != 0 && inject<Ban>().acceptConnection(remote_ip)) {
			Service_ptr service = services.front();
			if (service->is_single_socket()) {
				connection->accept(service->make_protocol(connection));
			} else {
				connection->accept();
			}
		} else {
			connection->close(FORCE_CLOSE);
		}

		accept();
	} else if (error != asio::error::operation_aborted) {
		if (!pendingStart) {
			close();
			pendingStart = true;
			deadline_timer.expires_from_now(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
			deadline_timer.async_wait(
				[self = shared_from_this(), serverPort = serverPort](const std::error_code &error) {
					if (!error) {
						openAcceptor(std::weak_ptr<ServicePort>(self), serverPort);
					}
				}
			);
		}
	}
}

Protocol_ptr ServicePort::make_protocol(bool checksummed, NetworkMessage &msg, const Connection_ptr &connection) const {
	uint8_t protocolID = msg.getByte();
	for (auto &service : services) {
		if (protocolID != service->get_protocol_identifier()) {
			continue;
		}

		if ((checksummed && service->is_checksummed()) || !service->is_checksummed()) {
			return service->make_protocol(connection);
		}
	}
	return nullptr;
}

void ServicePort::onStopServer() {
	close();
}

void ServicePort::openAcceptor(const std::weak_ptr<ServicePort> &weak_service, uint16_t port) {
	if (auto service = weak_service.lock()) {
		service->open(port);
	}
}

void ServicePort::open(uint16_t port) {
	close();

	serverPort = port;
	pendingStart = false;

	try {
		if (g_configManager().getBoolean(BIND_ONLY_GLOBAL_ADDRESS, __FUNCTION__)) {
			acceptor = std::make_unique<asio::ip::tcp::acceptor>(io_context, asio::ip::tcp::endpoint(asio::ip::address(asio::ip::address_v4::from_string(g_configManager().getString(IP, __FUNCTION__))), serverPort));
		} else {
			acceptor = std::make_unique<asio::ip::tcp::acceptor>(io_context, asio::ip::tcp::endpoint(asio::ip::address(asio::ip::address_v4(INADDR_ANY)), serverPort));
		}

		acceptor->set_option(asio::ip::tcp::no_delay(true));

		accept();
	} catch (const std::system_error &e) {
		g_logger().warn("[ServicePort::open] - Error code: {}", e.what());

		pendingStart = true;

		deadline_timer.expires_from_now(std::chrono::seconds(15));
		deadline_timer.async_wait(
			[self_weak = std::weak_ptr<ServicePort>(shared_from_this()), port](const std::error_code &error) {
				if (!error) {
					openAcceptor(self_weak, port);
				}
			}
		);
	}
}

void ServicePort::close() {
	if (acceptor && acceptor->is_open()) {
		std::error_code error;
		acceptor->close(error);
	}
}

bool ServicePort::add_service(const Service_ptr &new_svc) {
	if (std::ranges::any_of(services, [](const Service_ptr &svc) { return svc->is_single_socket(); })) {
		return false;
	}

	services.push_back(new_svc);
	return true;
}
