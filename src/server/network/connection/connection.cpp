/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2024 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "pch.hpp"

#include "server/network/connection/connection.hpp"
#include "server/network/message/outputmessage.hpp"
#include "server/network/protocol/protocol.hpp"
#include "game/scheduling/dispatcher.hpp"

ConnectionManager &ConnectionManager::getInstance() {
	return inject<ConnectionManager>();
}

Connection_ptr ConnectionManager::createConnection(asio::io_context &io_context, const ConstServicePort_ptr &servicePort) {
	auto connection = std::make_shared<Connection>(io_context, servicePort);
	connections.emplace(connection);
	g_logger().error("connection: {}", connections.size());
	return connection;
}

void ConnectionManager::releaseConnection(const Connection_ptr &connection) {
	connections.erase(connection);
}

void ConnectionManager::closeAll() {
	connections.for_each([&](const Connection_ptr &connection) {
		if (connection->socket.is_open()) {
			try {
				std::error_code error;
				connection->socket.shutdown(asio::ip::tcp::socket::shutdown_both, error);
				if (error) {
					g_logger().error("[ConnectionManager::closeAll] - Failed to close connection, system error code {}", error.message());
				}
			} catch (const std::system_error &systemError) {
				g_logger().error("[ConnectionManager::closeAll] - Exception caught: {}", systemError.what());
			}
		}
	});

	connections.clear();
}

Connection::Connection(asio::io_context &initIoService, ConstServicePort_ptr initservicePort) :
	readTimer(initIoService),
	writeTimer(initIoService),
	service_port(std::move(initservicePort)),
	socket(initIoService) {
}

void Connection::close(bool force) {
	ConnectionManager::getInstance().releaseConnection(shared_from_this());

	ip.store(0);

	if (connectionState.load() == CONNECTION_STATE_CLOSED) {
		return;
	}
	connectionState.store(CONNECTION_STATE_CLOSED);

	if (protocol) {
		g_dispatcher().addEvent([protocol = protocol] { protocol->release(); }, "Protocol::release", std::chrono::milliseconds(CONNECTION_WRITE_TIMEOUT * 1000).count());
	}

	if (messageQueue.empty() || force) {
		closeSocket();
	}
}

void Connection::closeSocket() {
	if (!socket.is_open()) {
		return;
	}

	try {
		readTimer.cancel();
		writeTimer.cancel();
		socket.cancel();

		std::error_code error;
		socket.shutdown(asio::ip::tcp::socket::shutdown_both, error);
		if (error && error != asio::error::not_connected) {
			g_logger().error("[Connection::closeSocket] - Failed to shutdown socket: {}", error.message());
		}

		socket.close(error);
		if (error && error != asio::error::not_connected) {
			g_logger().error("[Connection::closeSocket] - Failed to close socket: {}", error.message());
		}
	} catch (const std::system_error &e) {
		g_logger().error("[Connection::closeSocket] - error closeSocket: {}", e.what());
	}
}

Connection::~Connection() {
	closeSocket();
}

void Connection::accept(Protocol_ptr protocolPtr) {
	connectionState = CONNECTION_STATE_IDENTIFYING;
	protocol = std::move(protocolPtr);
	g_dispatcher().addEvent([protocol = protocol] { protocol->onConnect(); }, "Protocol::onConnect", std::chrono::milliseconds(CONNECTION_WRITE_TIMEOUT * 1000).count());

	try {
		readTimer.expires_from_now(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		readTimer.async_wait([self = std::weak_ptr<Connection>(shared_from_this())](const std::error_code &error) { Connection::handleTimeout(self, error); });

		asio::async_read(socket, asio::buffer(msg.getBuffer(), HEADER_LENGTH), [self = shared_from_this()](const std::error_code &error, std::size_t N) { self->parseProxyIdentification(error); });
	} catch (const std::system_error &e) {
		g_logger().error("[Connection::acceptInternal] - Exception in async_read: {}", e.what());
		close(FORCE_CLOSE);
	}
}

void Connection::accept() {
	try {
		readTimer.expires_from_now(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		readTimer.async_wait([self = std::weak_ptr<Connection>(shared_from_this())](const std::error_code &error) { Connection::handleTimeout(self, error); });

		asio::async_read(socket, asio::buffer(msg.getBuffer(), HEADER_LENGTH), [self = shared_from_this()](const std::error_code &error, std::size_t N) { self->parseHeader(error); });
	} catch (const std::system_error &e) {
		g_logger().error("[Connection::acceptInternal] - Exception in async_read: {}", e.what());
		close(FORCE_CLOSE);
	}
}

void Connection::parseProxyIdentification(const std::error_code &error) {
	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	readTimer.cancel();

	if (error || connectionState.load() == CONNECTION_STATE_CLOSED) {
		if (error != asio::error::operation_aborted && error != asio::error::eof && error != asio::error::connection_reset) {
			g_logger().error("[Connection::parseProxyIdentification] - Read error: {}", error.message());
		}
		close(FORCE_CLOSE);
		return;
	}

	uint8_t* msgBuffer = msg.getBuffer();
	auto charData = static_cast<char*>(static_cast<void*>(msgBuffer));
	std::string serverName = g_configManager().getString(SERVER_NAME, __FUNCTION__) + "\n";
	if (connectionState.load() == CONNECTION_STATE_IDENTIFYING) {
		if (msgBuffer[1] == 0x00 || strncasecmp(charData, &serverName[0], 2) != 0) {
			// Probably not proxy identification so let's try standard parsing method
			connectionState.store(CONNECTION_STATE_OPEN);
			parseHeader(error);
			return;
		} else {
			size_t remainder = serverName.length() - 2;
			if (remainder > 0) {
				connectionState.store(CONNECTION_STATE_READINGS);
				try {
					readTimer.expires_from_now(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
					readTimer.async_wait([self = std::weak_ptr<Connection>(shared_from_this())](const std::error_code &error) { Connection::handleTimeout(self, error); });

					// Read the remainder of proxy identification
					asio::async_read(socket, asio::buffer(msg.getBuffer(), remainder), [self = shared_from_this()](const std::error_code &error, std::size_t N) { self->parseProxyIdentification(error); });
				} catch (const std::system_error &e) {
					g_logger().error("Connection::parseProxyIdentification] - error: {}", e.what());
					close(FORCE_CLOSE);
				}
				return;
			} else {
				connectionState.store(CONNECTION_STATE_OPEN);
			}
		}
	} else if (connectionState.load() == CONNECTION_STATE_READINGS) {
		size_t remainder = serverName.length() - 2;
		if (strncasecmp(charData, &serverName[2], remainder) == 0) {
			connectionState.store(CONNECTION_STATE_OPEN);
		} else {
			g_logger().error("Connection::parseProxyIdentification] Invalid Client Login! Server Name mismatch!");
			close(FORCE_CLOSE);
			return;
		}
	}

	readTimer.expires_from_now(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
	readTimer.async_wait([self = std::weak_ptr<Connection>(shared_from_this())](const std::error_code &error) { Connection::handleTimeout(self, error); });

	try {
		asio::async_read(socket, asio::buffer(msg.getBuffer(), HEADER_LENGTH), [self = shared_from_this()](const std::error_code &error, std::size_t N) { self->parseHeader(error); });
	} catch (const std::system_error &e) {
		g_logger().error("[Connection::acceptInternal] - Exception in async_read: {}", e.what());
		close(FORCE_CLOSE);
	}
}

void Connection::parseHeader(const std::error_code &error) {
	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	readTimer.cancel();

	if (error) {
		if (error != asio::error::operation_aborted && error != asio::error::eof && error != asio::error::connection_reset) {
			g_logger().debug("[Connection::parseHeader] - Read error: {}", error.message());
		}
		close(FORCE_CLOSE);
		return;
	} else if (connectionState.load() == CONNECTION_STATE_CLOSED) {
		return;
	}

	uint32_t timePassed = std::max<uint32_t>(1, (time(nullptr) - timeConnected) + 1);
	if ((++packetsSent / timePassed) > static_cast<uint32_t>(g_configManager().getNumber(MAX_PACKETS_PER_SECOND, __FUNCTION__))) {
		g_logger().warn("[Connection::parseHeader] - {} disconnected for exceeding packet per second limit.", convertIPToString(getIP()));
		close();
		return;
	}

	if (timePassed > 2) {
		timeConnected = time(nullptr);
		packetsSent = 0;
	}

	uint16_t size = msg.getLengthHeader();
	if (size == 0 || size > INPUTMESSAGE_MAXSIZE) {
		close(FORCE_CLOSE);
		return;
	}

	try {
		readTimer.expires_from_now(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		readTimer.async_wait([self = std::weak_ptr<Connection>(shared_from_this())](const std::error_code &error) { Connection::handleTimeout(self, error); });

		// Read packet content
		msg.setLength(size + HEADER_LENGTH);
		// Read the remainder of proxy identification
		asio::async_read(socket, asio::buffer(msg.getBodyBuffer(), size), [self = shared_from_this()](const std::error_code &error, std::size_t N) { self->parsePacket(error); });
	} catch (const std::system_error &e) {
		g_logger().error("[Connection::parseHeader] - error: {}", e.what());
		close(FORCE_CLOSE);
	}
}

void Connection::parsePacket(const std::error_code &error) {
	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	readTimer.cancel();

	if (error || connectionState.load() == CONNECTION_STATE_CLOSED) {
		if (error) {
			g_logger().error("[Connection::parsePacket] - Read error: {}", error.message());
		}
		close(FORCE_CLOSE);
		return;
	}

	bool skipReadingNextPacket = false;
	if (!receivedFirst) {
		// First message received
		receivedFirst = true;

		if (!protocol) {
			// Check packet checksum
			uint32_t checksum;
			if (int32_t len = msg.getLength() - msg.getBufferPosition() - CHECKSUM_LENGTH;
				len > 0) {
				checksum = adlerChecksum(msg.getBuffer() + msg.getBufferPosition() + CHECKSUM_LENGTH, len);
			} else {
				checksum = 0;
			}

			auto recvChecksum = msg.get<uint32_t>();
			if (recvChecksum != checksum) {
				// it might not have been the checksum, step back
				msg.skipBytes(-CHECKSUM_LENGTH);
			}

			// Game protocol has already been created at this point
			protocol = service_port->make_protocol(recvChecksum == checksum, msg, shared_from_this());
			if (!protocol) {
				close(FORCE_CLOSE);
				return;
			}
		} else {
			// It is rather hard to detect if we have checksum or sequence method here so let's skip checksum check
			// it doesn't generate any problem because olders protocol don't use 'server sends first' feature
			msg.get<uint32_t>();
			// Skip protocol ID
			msg.skipBytes(1);
		}

		protocol->onRecvFirstMessage(msg);
	} else {
		// Send the packet to the current protocol
		skipReadingNextPacket = protocol->onRecvMessage(msg);
	}

	try {
		readTimer.expires_from_now(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		readTimer.async_wait([self = std::weak_ptr<Connection>(shared_from_this())](const std::error_code &error) { Connection::handleTimeout(self, error); });

		if (!skipReadingNextPacket) {
			// Wait to the next packet
			asio::async_read(socket, asio::buffer(msg.getBuffer(), HEADER_LENGTH), [self = shared_from_this()](const std::error_code &error, std::size_t N) { self->parseHeader(error); });
		}
	} catch (const std::system_error &e) {
		g_logger().error("[Connection::parsePacket] - error: {}", e.what());
		close(FORCE_CLOSE);
	}
}

void Connection::resumeWork() {
	try {
		asio::async_read(socket, asio::buffer(msg.getBuffer(), HEADER_LENGTH), [self = shared_from_this()](const std::error_code &error, std::size_t N) { self->parseHeader(error); });
	} catch (const std::system_error &e) {
		g_logger().error("[Connection::resumeWork] - Exception in async_read: {}", e.what());
		close(FORCE_CLOSE);
	}
}

void Connection::send(const OutputMessage_ptr &outputMessage) {
	if (connectionState.load() == CONNECTION_STATE_CLOSED) {
		return;
	}

	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	bool noPendingWrite = messageQueue.empty();
	messageQueue.emplace_back(outputMessage);

	if (noPendingWrite) {
		if (socket.is_open()) {
			try {
				asio::post(socket.get_executor(), [self = shared_from_this()] { self->internalWorker(); });
			} catch (const std::system_error &e) {
				g_logger().error("[Connection::send] - Exception in posting write operation: {}", e.what());
				close(FORCE_CLOSE);
			}
		} else {
			g_logger().error("[Connection::send] - Socket is not open for writing.");
			messageQueue.clear();
			close(FORCE_CLOSE);
		}
	}
}

void Connection::internalWorker() {
	if (messageQueue.empty()) {
		if (connectionState.load() == CONNECTION_STATE_CLOSED) {
			closeSocket();
		}
		return;
	}

	std::unique_lock<std::recursive_mutex> lockClass(connectionLock);
	const auto &outputMessage = messageQueue.front();
	lockClass.unlock();
	protocol->onSendMessage(outputMessage);
	lockClass.lock();

	internalSend(outputMessage);
}

uint32_t Connection::getIP() {
	if (ip.load() == 1) {
		std::error_code error;
		asio::ip::tcp::endpoint endpoint = socket.remote_endpoint(error);
		if (error) {
			g_logger().error("[Connection::getIP] - Failed to get remote endpoint: {}", error.message());
			ip.store(0);
		} else {
			uint32_t newIp = htonl(endpoint.address().to_v4().to_uint());
			ip.store(newIp);
		}
	}
	return ip.load();
}

void Connection::internalSend(const OutputMessage_ptr &outputMessage) {
	try {
		writeTimer.expires_from_now(std::chrono::seconds(CONNECTION_WRITE_TIMEOUT));
		writeTimer.async_wait([self = std::weak_ptr<Connection>(shared_from_this())](const std::error_code &error) { Connection::handleTimeout(self, error); });

		asio::async_write(socket, asio::buffer(outputMessage->getOutputBuffer(), outputMessage->getLength()), [self = shared_from_this()](const std::error_code &error, std::size_t N) { self->onWriteOperation(error); });
	} catch (const std::system_error &e) {
		g_logger().error("[Connection::internalSend] - Exception in async_write: {}", e.what());
		close(FORCE_CLOSE);
	}
}

void Connection::onWriteOperation(const std::error_code &error) {
	std::unique_lock<std::recursive_mutex> lockClass(connectionLock);
	writeTimer.cancel();
	messageQueue.pop_front();

	if (error) {
		g_logger().error("[Connection::onWriteOperation] - Write error: {}", error.message());
		messageQueue.clear();
		close(FORCE_CLOSE);
		return;
	}

	if (!messageQueue.empty()) {
		const auto &outputMessage = messageQueue.front();
		lockClass.unlock();
		protocol->onSendMessage(outputMessage);
		lockClass.lock();
		internalSend(outputMessage);
	} else if (connectionState == CONNECTION_STATE_CLOSED) {
		closeSocket();
	}
}

void Connection::handleTimeout(const ConnectionWeak_ptr &connectionWeak, const std::error_code &error) {
	if (error == asio::error::operation_aborted) {
		return;
	}

	if (auto connection = connectionWeak.lock()) {
		if (!error) {
			g_logger().debug("Connection Timeout, IP: {}", convertIPToString(connection->getIP()));
		} else {
			g_logger().debug("Connection Timeout or error: {}, IP: {}", error.message(), convertIPToString(connection->getIP()));
		}
		connection->close(FORCE_CLOSE);
	}
}
