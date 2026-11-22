#pragma once

#include <functional>
#include <string>

namespace HarkenSearch {

// Local pipe the external AI connector uses to ask Harken to write Playlists.
//
// The pipe only transports: it never touches the database. Requests are queued and
// executed on the UI thread by DrainExternalAiBridgeServer, so all database access
// stays where the rest of the app writes. One instance per library; starting a
// different library replaces the previous server.
void StartExternalAiBridgeServer(const std::string& libraryId);
void StopExternalAiBridgeServer();
bool ExternalAiBridgeServerRunning();

// Called once per frame on the UI thread. `handler` receives one request JSON and
// returns the response JSON.
void DrainExternalAiBridgeServer(const std::function<std::string(const std::string&)>& handler);

// "\\.\pipe\harken-external-ai-<library hash>" — exposed for tests and logs.
std::string ExternalAiBridgePipeName(const std::string& libraryId);

}  // namespace HarkenSearch
