"""
Simple iperf3 client over cellular connection
"""

import asyncio
import iperf3
import network
from walter_modem import Modem
from walter_modem.mixins.socket import *
from walter_modem.mixins.tls_certs import TLSCertsMixin, WalterModemTlsValidation, WalterModemTlsVersion
from walter_modem.mixins.default_sim_network import *
from walter_modem.mixins.default_pdp import *
from walter_modem.coreEnums import *

# ============== CONFIGURATION ==============
# Connection mode: "wifi" or "cellular"
CONNECTION_MODE = "wifi"

# WiFi settings
WIFI_SSID = "206W"
WIFI_PASSWORD = "kedaksku"

# Cellular settings
CELL_APN = ""
APN_USERNAME = ""
APN_PASSWORD = ""
SIM_PIN = None

# iperf3 settings
IPERF_SERVER_IP = "109.61.86.65" # Boston server from https://github.com/R0GGER/public-iperf3-servers

# ============== INITIALIZE MODEM (for cellular mode) ==============
modem = None
if CONNECTION_MODE == "cellular":
    modem = Modem(SocketMixin, TLSCertsMixin, load_default_power_saving_mixin=False)

# ============== HELPER FUNCTIONS ==============
async def wifi_connect(ssid, password, timeout=10):
    """Connect to WiFi network"""
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    
    if wlan.isconnected():
        print("Already connected to WiFi")
        print("IP:", wlan.ifconfig()[0])
        return True
    
    print(f"Connecting to WiFi: {ssid}...")
    wlan.connect(ssid, password)
    
    for _ in range(timeout):
        if wlan.isconnected():
            print("Connected to WiFi!")
            print("IP:", wlan.ifconfig()[0])
            return True
        await asyncio.sleep(1)
    
    print("WiFi connection timeout")
    return False

async def wait_for_network(timeout=180):
    for _ in range(timeout):
        state = modem.get_network_reg_state()
        if state in (WalterModemNetworkRegState.REGISTERED_HOME,
                     WalterModemNetworkRegState.REGISTERED_ROAMING):
            return True
        await asyncio.sleep(1)
    return False

async def lte_connect(retry=False):
    if modem.get_network_reg_state() in (
        WalterModemNetworkRegState.REGISTERED_HOME,
        WalterModemNetworkRegState.REGISTERED_ROAMING,
    ):
        return True

    if not await modem.set_op_state(WalterModemOpState.FULL):
        return False

    if not await modem.set_network_selection_mode(WalterModemNetworkSelMode.AUTOMATIC):
        return False

    print("Waiting for cellular network...")
    if not await wait_for_network(180):
        if not retry and await modem.get_rat(rsp=modem_rsp):
            next_rat = (WalterModemRat.NBIOT 
                       if modem_rsp.rat == WalterModemRat.LTEM 
                       else WalterModemRat.LTEM)
            print(f"Switching to {WalterModemRat.get_value_name(next_rat)}...")
            await modem.set_rat(next_rat)
            await modem.reset()
            return await lte_connect(retry=True)
        return False
    
    print("Connected!")
    return True

# ============== MAIN ==============
async def main():
    print("\n" + "="*50)
    print(f"iperf3 Client - {CONNECTION_MODE.upper()} Mode")
    print("="*50)
    
    # Connect based on mode
    if CONNECTION_MODE == "wifi":
        if not await wifi_connect(WIFI_SSID, WIFI_PASSWORD):
            print("WiFi connection failed!")
            return
    else:
        # cellular
        # Initialize modem
        await modem.begin()
        
        if not await modem.check_comm():
            print("Modem communication failed!")
            return
        print("Modem OK")
        
        # Unlock SIM if needed
        if SIM_PIN and not await modem.unlock_sim(pin=SIM_PIN):
            print("SIM unlock failed!")
            return
        
        # Create PDP context
        if not await modem.create_PDP_context(apn=CELL_APN):
            print("PDP context creation failed!")
            return
        
        # Set auth if needed
        if APN_USERNAME:
            await modem.set_PDP_auth_params(
                protocol=WalterModemPDPAuthProtocol.PAP,
                user_id=APN_USERNAME,
                password=APN_PASSWORD
            )
        
        # Connect to cellular network
        if not await lte_connect():
            print("Cellular connection failed!")
            return
    
    # Run iperf3 client
    print(f"\nConnecting to iperf3 server at {IPERF_SERVER_IP}...")
    print("Make sure your server is running: iperf3 -s")
    iperf3.client(IPERF_SERVER_IP)
    
    print("\nDone!")

asyncio.run(main())
