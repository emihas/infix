#!/usr/bin/env python3

import infamy

new_ip_address = "10.0.1.166"
new_prefix_lenght = 24
interface_name = "e0"

def print_ip_addresses(target):
    running = target.get_config_dict("ietf-interfaces:interfaces")
    interfaces = running.get("interfaces", {}).get("interface", [])

    if isinstance(interfaces, dict):
        interfaces = [interfaces]  # Convert single interface to list

    for interface in interfaces:
        name = interface["name"]

        if "ipv4" in interface:
            ipv4_addresses = interface["ipv4"].get("address", [])
            for ipv4_address in ipv4_addresses:
                address = ipv4_address.get("ip")
                prefix_length = ipv4_address.get("prefix-length")
                print(f"Interface: {name}, IPv4 Address: {address}/{prefix_length}")
        else:
            print(f"Interface: {name}, No IPv4 Address")

        if "ipv6" in interface:
            ipv6_addresses = interface["ipv6"].get("address", [])
            for ipv6_address in ipv6_addresses:
                address = ipv6_address.get("ip")
                prefix_length = ipv6_address.get("prefix-length")
                print(f"Interface: {name}, IPv6 Address: {address}/{prefix_length}")
        else:
            print(f"Interface: {name}, No IPv6 Address")
            
def check_ip_address_in_interface(ip_address, interface_name, prefix_length):
    running = target.get_config_dict("ietf-interfaces:interfaces")
    interfaces = running.get("interfaces", {}).get("interface", [])

    if isinstance(interfaces, dict):
        interfaces = [interfaces]
    
    found = False
    
    for interface in interfaces:
        name = interface["name"]

        if name == interface_name:
            if "ipv4" in interface:
                ipv4_addresses = interface["ipv4"].get("address", [])
                for ipv4_address in ipv4_addresses:
                    address = ipv4_address.get("ip")
                    length = ipv4_address.get("prefix-length")
                    if address == ip_address and length == prefix_length:
                        found = True
                        break

    assert found, f"IP address {ip_address}/{prefix_length} not found for interface {interface_name}"

            
def assert_ip_address_exists(ip_address):
    running = target.get_config_dict("ietf-interfaces:interfaces")
    interfaces = running.get("interfaces", {}).get("interface", [])

    found = False

    for interface in interfaces:
        if "ipv4" in interface:
            ipv4_addresses = interface["ipv4"].get("address", [])
            for ipv4_address in ipv4_addresses:
                address = ipv4_address.get("ip")
                if address == ip_address:
                    found = True
                    break

    assert found, f"IP address {ip_address} not found in target.get_config_dict('ietf-interfaces:interfaces')"

with infamy.Test() as test:
    with test.step("Setup"):
        env = infamy.Env(infamy.std_topology("1x1"))
        target = env.attach("target", "mgmt")

    with test.step("Get initial IP addresses"):
        print_ip_addresses(target)

    with test.step("Configure IP address"):
        config = {
            "interfaces": {
                "interface": [{
                    "name": "e0",
                    "type": "iana-if-type:ethernetCsmacd",
                    "ipv4": {
                        "address": [{
                            "ip": "10.0.1.176",
                            "prefix-length": 24
                        }]
                    }
                }]
            }
        }
        
        target.put_config_dict("ietf-interfaces", config)

    with test.step("Get updated IP addresses"):
        print_ip_addresses(target)
        check_ip_address_in_interface(new_ip_address, interface_name, new_prefix_lenght)
    
            
    test.succeed()