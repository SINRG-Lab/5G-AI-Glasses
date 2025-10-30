/**
 * @file WalterHTTPS.h
 * @author Arnoud Devoogdt <arnoud@dptechnics.com>
 * @date 11 September 2025
 * @copyright DPTechnics bv
 * @brief HTTPS Functions for Walter Modem
 *
 * @section LICENSE
 *
 * Copyright (C) 2025, DPTechnics bv
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of DPTechnics bv nor the names of its contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *   4. This software, with or without modification, must only be used with a
 *      Walter board from DPTechnics bv.
 *
 *   5. Any software provided in binary form under this license must not be
 *      reverse engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY DPTECHNICS BV "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL DPTECHNICS BV OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @section DESCRIPTION
 *
 * This file contains HTTPS POST functions extracted from the Walter Modem
 * library example code.
 * 
 * @note This code was originally derived from an example of how to connect to 
 * send HTTPS POST and GET requests within the Arduino IDE. It has been been 
 * modified to work with ESP-IDF in VSCode. 
 */

#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "WalterModem.h"
#include "WalterCOMM.h"

// Local esponse object for Modem
static WalterModemRsp rsp = {};

// Local buffer for incoming HTTPS response
static uint8_t incomingBuf[1024] = { 0 };

static const char *TAG = "COM";

static const char ca_cert[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

/**
 * @brief Common routine to wait for and print an HTTP response.
 */
static bool waitForHttpsResponse(uint8_t profile, const char* contentType) 
{
  ESP_LOGI(TAG, "Waiting for reply...");
  const uint16_t maxPolls = 30;
  for(uint16_t i = 0; i < maxPolls; i++) {
    if(comm::modem.httpDidRing(profile, incomingBuf, sizeof(incomingBuf), &rsp)) {
      ESP_LOGI(TAG, "HTTPS status code (Modem): %d", rsp.data.httpResponse.httpStatus);
      ESP_LOGI(TAG, "Content type: %s", contentType);
      ESP_LOGI(TAG, "Payload:\n%s", incomingBuf);
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGE(TAG, "HTTPS response timeout");
  return false;
}

namespace comm{
  WalterModem modem;

  /**
   * @brief This function checks if we are connected to the LTE network
   *
   * @return true when connected, false otherwise
   */
  bool lteConnected() {
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    return (regState == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
            regState == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
  }

  /**
   * @brief This function waits for the modem to be connected to the LTE network.
   *
   * @param timeout_sec The amount of seconds to wait before returning a time-out.
   *
   * @return true if connected, false on time-out.
   */
  bool waitForNetwork(int timeout_sec) 
  {
    ESP_LOGI(TAG, "Connecting to the network...");
    int time = 0;
    while(!lteConnected()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      time++;
      if(time > timeout_sec) {
        return false;
      }
    }
    ESP_LOGI(TAG, "Connected to the network");
    return true;
  }

  /**
   * @brief Disconnect from the LTE network.
   *
   * This function will disconnect the modem from the LTE network and block until
   * the network is actually disconnected. After the network is disconnected the
   * GNSS subsystem can be used.
   *
   * @return true on success, false on error.
   */
  bool lteDisconnect() 
  {
    // Set the operational state to minimum
    if(modem.setOpState(WALTER_MODEM_OPSTATE_MINIMUM)) {
      ESP_LOGI(TAG, "Successfully set operational state to MINIMUM");
    } else {
      ESP_LOGE(TAG, "Could not set operational state to MINIMUM");
      return false;
    }

    // Wait for the network to become available
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    while(regState != WALTER_MODEM_NETWORK_REG_NOT_SEARCHING) {
      vTaskDelay(pdMS_TO_TICKS(100));
      regState = modem.getNetworkRegState();
    }

    ESP_LOGI(TAG, "Disconnected from the network");
    return true;
  }

  /**
   * @brief This function tries to connect the modem to the cellular network.
   *
   * @return true on success, false on error.
   */
  bool lteConnect() 
  {
    // Set the operational state to NO RF
    if(modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
      ESP_LOGI(TAG, "Successfully set operational state to NO RF");
    } else {
      ESP_LOGE(TAG, "Could not set operational state to NO RF");
      return false;
    }

    // Create PDP context
    if(modem.definePDPContext()) {
      ESP_LOGI(TAG, "Created PDP context");
    } else {
      ESP_LOGE(TAG, "Could not create PDP context");
      return false;
    }

    // Set the operational state to full
    if(modem.setOpState(WALTER_MODEM_OPSTATE_FULL)) {
      ESP_LOGI(TAG, "Successfully set operational state to FULL");
    } else {
      ESP_LOGE(TAG, "Could not set operational state to FULL");
      return false;
    }

    // Set the network operator selection to automatic
    if(modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
      ESP_LOGI(TAG, "Network selection mode was set to automatic");
    } else {
      ESP_LOGE(TAG, "Could not set the network selection mode to automatic");
      return false;
    }

    return waitForNetwork(300);
  }

  /**
   * @brief Writes TLS credentials to the modem's NVS and configures the TLS profile.
   *
   * This function stores the provided TLS certificates and private keys into the modem's
   * non-volatile storage (NVS), and then sets up a TLS profile for secure communication.
   * These configuration changes are persistent across reboots.
   *
   * @note
   * - Certificate indexes 0-10 are reserved for Sequans and BlueCherry internal usage.
   * - Private key index 1 is reserved for BlueCherry internal usage.
   * - Do not attempt to override or use these reserved indexes.
   *
   * @return
   * - true if the credentials were successfully written and the profile configured.
   * - false otherwise.
   */
  bool setupTLSProfile(int https_tls_profile) 
  {
    if(!modem.tlsWriteCredential(false, 12, ca_cert)) {
      ESP_LOGE(TAG, "CA cert upload failed");
      return false;
    }

    if(modem.tlsConfigProfile(https_tls_profile, WALTER_MODEM_TLS_VALIDATION_CA,
                              WALTER_MODEM_TLS_VERSION_12, 12)) {
      ESP_LOGI(TAG, "TLS profile configured");
    } else {
      ESP_LOGE(TAG, "TLS profile configuration failed");
      return false;
    }

    return true;
  }

  /**
   * @brief Perform an HTTPS POST request with a body.
   */
  bool httpsPost(
      const char* path, 
      const uint8_t* body, 
      size_t bodyLen, 
      const char* mimeType, 
      int modem_https_profile, 
      const char* https_host) 
  {
    char ctBuf[32] = { 0 };

    ESP_LOGI(TAG, "Sending HTTPS POST to %s%s (%u bytes, type %s)", https_host, path,
                  (unsigned) bodyLen, mimeType);
    if(!modem.httpSend(modem_https_profile, path, (uint8_t*) body, (uint16_t) bodyLen,
                      WALTER_MODEM_HTTP_SEND_CMD_POST, WALTER_MODEM_HTTP_POST_PARAM_JSON, ctBuf,
                      sizeof(ctBuf))) {
      ESP_LOGE(TAG, "HTTPS POST failed");
      return false;
    }
    ESP_LOGI(TAG, "HTTPS POST successfully sent");
    return waitForHttpsResponse(modem_https_profile, ctBuf);
  }
}