This guide applies to Debian-based Linux distributions only.
# Bridging the VM to the physical network
By creating a tap interface tied to a bridge on a Debian machine, you can bridge your IntelliStar VM to network as physically as possible. The advantages you'll get from it include full network connectivity to the IntelliStar, meaning you can talk to it natively over your LAN and reap the benefits of SFTP, SSH, ICMP, UDP data and so forth... The biggest take away here is that this enables the VM to receive multicast UDP data, pending that you've actually configured the **em0** interface as your multicast route in /etc/rc.conf.

***Fair warning: This guide kind of expects that you understand general TCP/IP concepts and navigation of a Linux file system. There will be less hand-holding.***

**It also requires that you *READ* instead of just copy and pasting the code blocks into your terminal. *READ ALL OF THE INSTRUCTIONS.***

# Step 1: Create the bridge adapter and tap
First, we need to create a bridge network interface that we can assign the tap to. **Find the name of the network interface you intend to allocate for VM usage, and record the name of it, as we are about to use it.** It will be referred to as `<interface name here>` in the following instructions.

Once you have the name of the interface you'd like to use, proceed with initializing the bridge and the tap:
```bash
ip link add name br0 type bridge
ip link set dev <interface name here> master br0
ip link set dev br0 up
ip tuntap add dev tap0 mode tap user $USER
ip link set dev tap0 master br0
ip link set dev tap0 up
```
You may lose networking. Proceed, this is expected for now.

If using a GUI and this is your only network interface, you may notice the GUI element for network interface management disappear. That's fine.

Okay, now we need to edit the **netplan config**, which we'll find at `/etc/netplan/50-cloud-init.yaml`. Open it in your favorite text editor.

You should already have the first couple of lines (aside from `renderer: networkd` at the start) in the YAML. **Change dhcp4 on your interface to <u>false</u>** (we fix this on the create **br0** bridge). Then, update the YAML to register the **tap0** interface we created within **ethernets**. Then, define **br0** under **bridges**, and tie it to your desired interface, adding the **tap0** interface to it. Set **dhcp4** to true if you prefer DHCP. Add the final parameters as seen. Your YAML should look practically identical to the one pictured below.
```yaml
network:
  renderer: networkd
  version: 2
  ethernets:
    <interface name here>:
      dhcp4: false
      dhcp6: false
    tap0:
      dhcp4: false
      dhcp6: false
      optional: true
  bridges:
    br0:
      interfaces:
        - <interface name here>
        - tap0
      dhcp4: true
      dhcp6: false
      parameters:
        stp: false
        forward-delay: 0
```
Okay, now we need to register the **tap** as a systemd netdev. Create a new file for the tap at `/etc/systemd/network/25-tap0.netdev`. Use your favorite text editor to define it as follows, replacing `<your user>` with the **username of the user that will be running the VM**:
```ini
[NetDev]
Name=tap0
Kind=tap

[Tap]
User=<your user>
```
Save the file.

Okay, great! Now, if we did this correctly, we should be able to restart networkd and apply the netplan:
```bash
sudo systemctl restart systemd-networkd
sudo netplan apply
```
Your networking should return, if it was temporarily rendered disabled. Make sure you can ping something. If you cannot, or your networking did NOT return, verify that your configuration is correct without typos.

# Step 2: Assign the tap to your VM
If the network bridge is working, our next step is to modify our VM's **run.sh** script to use the tap interface instead of forwarding.

Locate your VM's **run.sh** script. If unmodified from original setup, you should have a portion that looks like this:
```bash
-netdev user,id=net0,net=10.100.102.0/24,host=10.100.102.1 \
-device i82557b,netdev=net0 \
-netdev user,id=net1,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22 \
-device e1000-82545em,netdev=net1 \
```
If so, then we're going to modify the **net1** interface to use our **tap0** adapter. Using your favorite text editor, change the arguments to the following:
```bash
-netdev tap,id=net1,ifname=tap0,script=no,downscript=no,vnet_hdr=off \
-device e1000-82545em,netdev=net1 \
```
By assigning **tap0** to the **ifname**, **tap0** will be exclusively used on this VM. ***This means only the VM can use the tap0 interface while it is running, so do not expect to run multiple VMs on the same tap.***

Okay, cool! The VM now has a network adapter that can physically reach your network. All that's left to do is configure the VM's network settings...

# Step 3: Configure VM's IP address
Go ahead and start up your VM. Open up `/etc/rc.conf` with **ee** (or **vi** if you are a masochist).

Set **em0** to a static address on your network subnet:
```
ifconfig_em0="inet <static ip> netmask <subnet mask>"
static_routes="mcast"
route_mcast="224.0.0/8 -interface em0"
```
Make sure you have that last bit for setting the multicast inbound route to **em0**.

Keep in mind you may also have to adjust `/usr/local/bin/prov_netconf` if your VM regenerates the **rc.conf**.

Well, if you've configured a valid static IP, go ahead and give your VM a good old `reboot now` and you should be alive on the network in no time!