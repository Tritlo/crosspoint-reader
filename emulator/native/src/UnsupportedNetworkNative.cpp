#include <iostream>

#include "network/CrossPointWebServer.h"

CrossPointWebServer::CrossPointWebServer() = default;
CrossPointWebServer::~CrossPointWebServer() = default;

void CrossPointWebServer::begin() {
  running = false;
  std::cerr << "emulator: web server is not supported\n";
}

void CrossPointWebServer::stop() { running = false; }
void CrossPointWebServer::handleClient() {}
CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const { return {}; }
