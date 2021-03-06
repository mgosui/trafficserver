/** @file

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include "ts/ink_config.h"

#include "P_Net.h"
#include "ts/I_Layout.h"
#include "I_RecHttp.h"
#include "P_SSLUtils.h"
#include "P_OCSPStapling.h"

//
// Global Data
//

SSLNetProcessor ssl_NetProcessor;
NetProcessor &sslNetProcessor = ssl_NetProcessor;

#ifdef HAVE_OPENSSL_OCSP_STAPLING
struct OCSPContinuation : public Continuation {
  int
  mainEvent(int /* event ATS_UNUSED */, Event * /* e ATS_UNUSED */)
  {
    ocsp_update();

    return EVENT_CONT;
  }

  OCSPContinuation() : Continuation(new_ProxyMutex()) { SET_HANDLER(&OCSPContinuation::mainEvent); }
};
#endif /* HAVE_OPENSSL_OCSP_STAPLING */

void
SSLNetProcessor::cleanup(void)
{
  SSLReleaseContext(client_ctx);
  client_ctx = NULL;
}

int
SSLNetProcessor::start(int, size_t stacksize)
{
  // This initialization order matters ...
  SSLInitializeLibrary();
  SSLConfig::startup();

  if (!SSLCertificateConfig::startup())
    return -1;

  // Acquire a SSLConfigParams instance *after* we start SSL up.
  SSLConfig::scoped_config params;

  // Enable client regardless of config file settings as remap file
  // can cause HTTP layer to connect using SSL. But only if SSL
  // initialization hasn't failed already.
  client_ctx = SSLInitClientContext(params);
  if (!client_ctx) {
    SSLError("Can't initialize the SSL client, HTTPS in remap rules will not function");
  }

  // Initialize SSL statistics. This depends on an initial set of certificates being loaded above.
  SSLInitializeStatistics();

#ifdef HAVE_OPENSSL_OCSP_STAPLING
  if (SSLConfigParams::ssl_ocsp_enabled) {
    EventType ET_OCSP = eventProcessor.spawn_event_threads(1, "ET_OCSP", stacksize);
    eventProcessor.schedule_every(new OCSPContinuation(), HRTIME_SECONDS(SSLConfigParams::ssl_ocsp_update_period), ET_OCSP);
  }
#endif /* HAVE_OPENSSL_OCSP_STAPLING */

  // We have removed the difference between ET_SSL threads and ET_NET threads,
  // So just keep on chugging
  return 0;
}

NetAccept *
SSLNetProcessor::createNetAccept()
{
  return (NetAccept *)new SSLNetAccept;
}

NetVConnection *
SSLNetProcessor::allocate_vc(EThread *t)
{
  SSLNetVConnection *vc;

  if (t) {
    vc = THREAD_ALLOC_INIT(sslNetVCAllocator, t);
  } else {
    if (likely(vc = sslNetVCAllocator.alloc())) {
      vc->from_accept_thread = true;
    }
  }

  return vc;
}

SSLNetProcessor::SSLNetProcessor() : client_ctx(NULL)
{
}

SSLNetProcessor::~SSLNetProcessor()
{
  cleanup();
}
