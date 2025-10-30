/**
 * @brief Header file for HTTPS and LTE connect functions derived from the DPTechnics 
 * Arduino demo of HTTPS. See WalterHTTPS.cpp for copyright.
 */

 namespace comm{
  // Modem Object
  extern WalterModem modem;

  /**
   * @brief This function checks if we are connected to the LTE network
   *
   * @return true when connected, false otherwise
   */
  bool lteConnected();

  /**
   * @brief This function waits for the modem to be connected to the LTE network.
   *
   * @param timeout_sec The amount of seconds to wait before returning a time-out.
   *
   * @return true if connected, false on time-out.
   */
  bool waitForNetwork(int timeout_sec);

  /**
   * @brief Disconnect from the LTE network.
   *
   * This function will disconnect the modem from the LTE network and block until
   * the network is actually disconnected. After the network is disconnected the
   * GNSS subsystem can be used.
   *
   * @return true on success, false on error.
   */
  bool lteDisconnect();

  /**
   * @brief This function tries to connect the modem to the cellular network.
   *
   * @return true on success, false on error.
   */
  bool lteConnect();

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
  bool setupTLSProfile(int https_tls_profile);

  /**
   * @brief Perform an HTTPS POST request with a body.
   */
  bool httpsPost(const char* path, const uint8_t* body, size_t bodyLen,
                const char* mimeType, int modem_https_profile, 
                const char* https_host);
}