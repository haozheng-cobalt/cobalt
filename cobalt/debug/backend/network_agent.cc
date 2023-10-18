// Copyright 2023 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cobalt/debug/backend/network_agent.h"

#include <set>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/values.h"
#include "cobalt/debug/console/command_manager.h"
#include "cobalt/debug/json_object.h"

namespace cobalt {
namespace debug {
namespace backend {

NetworkAgent::NetworkAgent(
    DebugDispatcher* dispatcher, dom::Window* window,
    network::NetworkModule::UrlFetchCallbacks* url_fetch_callbacks)
    : AgentBase("Network", dispatcher),
      window_(window),
      url_fetch_callbacks_(url_fetch_callbacks) {
  LOG(INFO) << "NetworkAgent::NetworkAgent";
#if 0
     commands_["enable"] =
         base::Bind(&NetworkAgent::Enable, base::Unretained(this));
     commands_["disable"] =
         base::Bind(&NetworkAgent::Disable, base::Unretained(this));
#endif
  url_fetch_callbacks->start =
      base::Bind(&NetworkAgent::Onstart, base::Unretained(this));
}

void NetworkAgent::Onstart(net::URLFetcher* url_fetcher) {
  LOG(INFO) << "NetworkAgent::Onstart url_fetcher " << url_fetcher
            << " url_fetch_callbacks_" << url_fetch_callbacks_;
  JSONObject params(new base::DictionaryValue());
  // JSONObject request_params(new base::DictionaryValue());
  std::unique_ptr<base::DictionaryValue> request_params(
      new base::DictionaryValue);

  params->SetInteger("requestId", reinterpret_cast<int>(1));
  params->SetInteger("loaderId", reinterpret_cast<int>(1));
  request_params->SetString("documentURL",
                            url_fetcher->GetOriginalURL().spec());

  request_params->SetString("url", url_fetcher->GetOriginalURL().spec());
  request_params->SetString("method", "GET");
  params->SetDictionary("request", std::move(request_params));

  params->SetInteger("timestamp", reinterpret_cast<int>(1));
  params->SetInteger("wallTime", reinterpret_cast<int>(1));
  params->SetString("initiator", "foo");
  params->SetString("type", "XHR");

  dispatcher_->SendEvent(domain_ + ".requestWillBeSent", params);
  dispatcher_->SendEvent(domain_ + ".responseReceived", params);
  dispatcher_->SendEvent(domain_ + ".dataReceived", params);
  dispatcher_->SendEvent(domain_ + ".loadingFinished", params);
}

bool NetworkAgent::DoEnable(Command* command) {
  LOG(INFO) << "NetworkAgent::DoEnable";
  if (!AgentBase::DoEnable(command)) return false;

  // Send an executionContextCreated event.
  // https://chromedevtools.github.io/devtools-protocol/1-3/Runtime#event-executionContextCreated
  JSONObject params(new base::DictionaryValue());
  params->SetInteger("context.id", 1);
  params->SetString("context.origin", window_->location()->origin());
  params->SetString("context.name", "Cobalt");
  params->SetBoolean("context.auxData.isDefault", true);
  return true;
}


}  // namespace backend
}  // namespace debug
}  // namespace cobalt
