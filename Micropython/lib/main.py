"""
Walter ESP32-S3 MicroPython - Gemini API via Raw Sockets
Uses socket mixin to bypass HTTP mixin's chunked encoding limitations
"""

import micropython
micropython.opt_level(1)

import asyncio
import sys
import ujson as json
from walter_modem import Modem
from walter_modem.mixins.default_sim_network import *
from walter_modem.mixins.default_pdp import *
from walter_modem.mixins.socket import *
from walter_modem.mixins.tls_certs import TLSCertsMixin, WalterModemTlsValidation, WalterModemTlsVersion
from walter_modem.coreEnums import *
from walter_modem.coreStructs import *

# ============== CONFIGURATION ==============
GEMINI_API_KEY = "AIzaSyCsaor8fHFd1bLJJ1HTrxzfqL-Q70Xigwo"  
CELL_APN = ""
APN_USERNAME = ""
APN_PASSWORD = ""
SIM_PIN = None

# ============== INITIALIZE MODEM ==============
modem = Modem(SocketMixin, TLSCertsMixin, load_default_power_saving_mixin=False)
modem_rsp = WalterModemRsp()

# ============== HELPER FUNCTIONS ==============
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

# ============== RESPONSE PARSER ==============
def try_parse_response(response_data):
    """Try to parse HTTP response with chunked encoding support"""
    try:
        header_end = response_data.find(b'\r\n\r\n')
        if header_end < 0:
            return None
        
        body = response_data[header_end+4:]
        
        # Handle chunked transfer encoding
        if b'Transfer-Encoding: chunked' in response_data[:header_end]:
            decoded_body = b''
            chunk_data = body
            while chunk_data:
                newline_pos = chunk_data.find(b'\r\n')
                if newline_pos == -1:
                    break
                size_str = chunk_data[:newline_pos].decode().strip()
                if not size_str:
                    break
                try:
                    chunk_size = int(size_str, 16)
                except:
                    break
                if chunk_size == 0:
                    break
                chunk_start = newline_pos + 2
                chunk_end = chunk_start + chunk_size
                if chunk_end > len(chunk_data):
                    decoded_body += chunk_data[chunk_start:]
                    break
                decoded_body += chunk_data[chunk_start:chunk_end]
                chunk_data = chunk_data[chunk_end+2:]
            body = decoded_body
        
        json_start = body.find(b'{')
        if json_start < 0:
            return None
        
        json_bytes = body[json_start:]
        json_str = json_bytes.decode('utf-8')
        data = json.loads(json_str)
        return data["candidates"][0]["content"]["parts"][0]["text"]
    except:
        return None

# ============== GEMINI API QUERY ==============
async def query_gemini(prompt, max_tokens=256):
    server = "generativelanguage.googleapis.com"
    port = 443
    uri = f"/v1beta/models/gemini-2.5-flash:generateContent?key={GEMINI_API_KEY}"
    
    # Build JSON payload
    payload_dict = {
        "contents": [{"parts": [{"text": prompt}]}],
        "generationConfig": {"maxOutputTokens": max_tokens, "temperature": 0.7}
    }
    body = json.dumps(payload_dict)
    
    # Build raw HTTP request
    http_request = (
        f"POST {uri} HTTP/1.1\r\n"
        f"Host: {server}\r\n"
        f"Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Connection: keep-alive\r\n"
        f"Accept: application/json\r\n"
        f"\r\n"
        f"{body}"
    )
    
    print(f"\nSending: {prompt[:50]}..." if len(prompt) > 50 else f"\nSending: {prompt}")
    
    socket_id = 1
    
    # Configure TLS profile
    print("Configuring TLS...")
    if not await modem.tls_config_profile(
        profile_id=1,
        tls_version=WalterModemTlsVersion.TLS_VERSION_12,
        tls_validation=WalterModemTlsValidation.NONE
    ):
        print("TLS config failed")
        return None
    
    # Configure socket
    print("Configuring socket...")
    if not await modem.socket_config(
        ctx_id=socket_id,
        pdp_ctx_id=1,
        mtu=1500,
        exchange_timeout=90,
        connection_timeout=60,
        send_delay_ms=100
    ):
        print("Socket config failed")
        return None
    
    # Enable TLS on socket
    print("Enabling TLS on socket...")
    if not await modem.socket_config_secure(
        ctx_id=socket_id,
        enable=True,
        secure_profile_id=1
    ):
        print("Socket TLS config failed")
        return None
    
    # Connect to server
    print(f"Connecting to {server}:{port}...")
    if not await modem.socket_dial(
        ctx_id=socket_id,
        remote_addr=server,
        remote_port=port,
        protocol=WalterModemSocketProtocol.TCP
    ):
        print("Socket dial failed")
        return None
    
    print("Connected, sending request...")
    
    # Send HTTP request
    if not await modem.socket_send(
        ctx_id=socket_id,
        data=http_request.encode('utf-8')
    ):
        print("Send failed")
        await modem.socket_close(ctx_id=socket_id)
        return None
    
    print("Request sent, waiting for response...")
    await asyncio.sleep(2)
    
    # Receive response
    response_data = b''
    no_data_count = 0
    
    for i in range(90):
        try:
            rings = modem.socket_context_states[socket_id].rings
            
            if rings:
                ring = rings.pop(0)
                data_length = ring.length if ring.length else 1500
                print(f"  Ring: {data_length} bytes")
                
                result = await modem.socket_receive_data(
                    ctx_id=socket_id,
                    length=data_length,
                    max_bytes=min(data_length, 1500),
                    rsp=modem_rsp
                )
                
                if modem_rsp.socket_rcv_response:
                    payload = modem_rsp.socket_rcv_response.payload
                    if payload:
                        response_data += bytes(payload)
                        print(f"  +{len(payload)} bytes (total: {len(response_data)})")
                elif ring.data:
                    response_data += bytes(ring.data)
                    print(f"  Ring data: {len(ring.data)} bytes")
                
                no_data_count = 0
                
                # Check for end of chunked response
                if b'\r\n0\r\n' in response_data:
                    print("  End of chunked response detected")
                    result = try_parse_response(response_data)
                    if result:
                        try:
                            await modem.socket_close(ctx_id=socket_id)
                        except:
                            pass
                        return result
            else:
                no_data_count += 1
            
            # Try parsing periodically
            if response_data and no_data_count > 3:
                result = try_parse_response(response_data)
                if result:
                    try:
                        await modem.socket_close(ctx_id=socket_id)
                    except:
                        pass
                    return result
            
            if no_data_count > 20 and len(response_data) > 0:
                print(f"  No new data, attempting final parse...")
                break
            elif no_data_count > 40:
                print("Timeout")
                break
                
        except Exception as e:
            print(f"  Error: {e}")
        
        await asyncio.sleep(1)
        
        if i % 20 == 0 and i > 0:
            print(f"  Status: {i}s, {len(response_data)} bytes")
    
    # Final parse
    if response_data:
        print(f"Final: {len(response_data)} bytes")
        result = try_parse_response(response_data)
        if result:
            try:
                await modem.socket_close(ctx_id=socket_id)
            except:
                pass
            return result
        
        # Debug
        header_end = response_data.find(b'\r\n\r\n')
        if header_end > 0:
            body = response_data[header_end+4:]
            print(f"Body preview: {body[:300]}")
    
    try:
        await modem.socket_close(ctx_id=socket_id)
    except:
        pass
    return None

# ============== SETUP ==============
async def setup():
    print("\n" + "="*50)
    print("Walter - Gemini AI (Socket Mode)")
    print("="*50)
    
    await modem.begin()
    
    if not await modem.check_comm():
        print("Modem failed!")
        return False
    print("Modem OK")
    
    if SIM_PIN and not await modem.unlock_sim(pin=SIM_PIN):
        return False
    
    if not await modem.create_PDP_context(apn=CELL_APN):
        print("PDP context failed")
        return False
    
    if APN_USERNAME:
        await modem.set_PDP_auth_params(
            protocol=WalterModemPDPAuthProtocol.PAP,
            user_id=APN_USERNAME,
            password=APN_PASSWORD
        )
    
    if not await lte_connect():
        return False
    
    return True

# ============== MAIN ==============
async def main():
    try:
        if not await setup():
            raise RuntimeError("Setup failed")
        
        print("\nReady!")
        
        questions = ["What is biomedical engineering in one sentence?"]
        
        for i, question in enumerate(questions):
            print(f"\n--- Query {i+1} ---")
            response = await query_gemini(question)
            
            if response:
                print(f"\nGemini:\n{response}")
            else:
                print("Failed")
        
        print("\nDone!")
        
    except Exception as err:
        print("ERROR:")
        sys.print_exception(err)

asyncio.run(main())