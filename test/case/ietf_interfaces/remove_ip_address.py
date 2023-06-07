#!/usr/bin/env python3

import infamy

#Name of the interface to remove IPv4 addresses from:
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

def assert_ipv4_removed(target, interface_name):
    running = target.get_config_dict("ietf-interfaces:interfaces")
    interfaces = running.get("interfaces", {}).get("interface", [])

    if isinstance(interfaces, dict):
        interfaces = [interfaces]  # Convert single interface to list

    for interface in interfaces:
        name = interface["name"]
        if name == interface_name:
            assert "ipv4" not in interface, f"Interface {interface_name} doesn't have ipv4 addr!"
            return
    print(f"No interface with the name {interface_name}")
         
with infamy.Test() as test:
    with test.step("Setup"):
        env = infamy.Env(infamy.std_topology("1x1"))
        target = env.attach("target", "mgmt")

    with test.step("Get initial IP addresses"):
        print_ip_addresses(target)

    with test.step("Configure IP address"):
        config_xml = '''
                  <if:interfaces xmlns:if="urn:ietf:params:xml:ns:yang:ietf-interfaces">
                    <if:interface>
                      <if:name>e0</if:name>
                      <ip:ipv4 xmlns:ip="urn:ietf:params:xml:ns:yang:ietf-ip" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" nc:operation="remove"/>
                      <if:type xmlns:iana-if-type="urn:ietf:params:xml:ns:yang:iana-if-type">iana-if-type:ethernetCsmacd</if:type>
                    </if:interface>
                  </if:interfaces>
                  '''
        target.ncc.edit_config(config_xml)

    with test.step("Get updated IP addresses"):
        print_ip_addresses(target)
        assert_ipv4_removed(target, interface_name)
    
            
    test.succeed()