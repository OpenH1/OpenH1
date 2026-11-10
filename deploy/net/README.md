# net.goath1.com: the project's broker and relay

One small server carries what internet play needs from the project:

- **an MQTT broker** on TCP 1883 (Mosquitto 2.0), the first in
  `port/assets/network/brokers.txt` and the game's `DEFAULT_BROKERS`
  (`port/linux/src/p2p_signal.c`): invites' signalling, parties, friends and
  the server browser's listings. Everything the game sends through it is
  sealed end to end, to topics that are hashes, so the broker learns
  nothing and needs no secrets;
- **a relay** on UDP 2302 (`halo --relay`, `docs/dedicated_server.md`,
  "Relays"), in `port/assets/network/relays.txt` and the game's
  `DEFAULT_RELAYS` (`port/linux/src/master_list.c`), for two machines whose
  NATs keep them apart.

The other brokers in the list (OpenCE's `opence.milenko.org`, which OpenCE's
players use, and a public one) carry the same messages, so the game goes on
if this server is down.

The server: a Hetzner Cloud CPX11 (2 vCPU, 2 GB) in Ashburn, Ubuntu 24.04,
`goath1-net-ash`; `net.goath1.com` has its A and AAAA records (DNS only, not
proxied). Its Hetzner Cloud firewall (`goath1-net`) lets in only TCP 1883,
UDP 2302 and ICMP from the internet.

**ssh is over Tailscale only**: the server is on the maintainer's tailnet as
`goath1-net-ash` (`ssh root@goath1-net-ash`), with key-only logins; port 22
is closed to the internet by the Hetzner firewall and dropped by the
server's own (`nftables-goath1.nft`), which accepts everything on
`tailscale0` and Tailscale's UDP 41641. If Tailscale is down, use the Hetzner
Cloud console (`hcloud server request-console goath1-net-ash`, or the web
console: a root password, `hcloud server reset-password`, then
`tailscale up` or a look at `journalctl -u tailscaled`), or boot the rescue
system (`hcloud server enable-rescue goath1-net-ash --ssh-key <key>`, then
`hcloud server reboot`) and mount the disk. Opening 22 in the Hetzner
firewall alone is not enough: the server's nftables drops it too (`nft
delete table inet goath1` from the console lets it in until the next
boot).

| File | Installed as | What it is |
| --- | --- | --- |
| `mosquitto.conf` | `/etc/mosquitto/mosquitto.conf` | The broker: anonymous, MQTT 5 and 3.1.1, the limits, retained listings kept through restarts |
| `goath1.acl` | `/etc/mosquitto/goath1.acl` | The game's topics, the only ones carried |
| `logrotate-mosquitto` | `/etc/logrotate.d/mosquitto` | The broker's log, daily, a week kept |
| `nftables-goath1.nft` | `/etc/nftables.d/goath1.nft` | The firewall: the four ports, new connections limited per address |
| `openh1-relay.service` | `/etc/systemd/system/` | `halo --relay`, as the user `openh1-relay` |
| `update-relay.sh` | `/usr/local/sbin/update-relay.sh` | The latest release installed as the relay, its signature checked |
| `openh1-relay-update.service`, `.timer` | `/etc/systemd/system/` | `update-relay.sh` weekly |
| `nftables.conf` | `/etc/nftables.conf` | Loads `/etc/nftables.d`, without flushing Tailscale's tables |
| `sshd-goath1.conf` | `/etc/ssh/sshd_config.d/10-goath1.conf` | ssh by key only. Already on the server: a copy of it, which `setup.sh` installs only on a new server that has none |
| `unattended-goath1.conf` | `/etc/apt/apt.conf.d/51goath1-reboot` | Security updates daily, rebooting at 09:30 UTC when they need to. Already on the server: `setup.sh` installs it only where it is missing |
| `setup.sh` | | All of the above on a fresh Ubuntu 24.04, as root |

## The broker

The game needs of a broker: retained messages, wildcard subscriptions
(`hceu/3/lobby/s/+`), wills (each connection's clears its slot's listing
when the connection dies), MQTT 5's message expiry (a listing lasts 90
seconds), QoS 1, and anonymous clients. Mosquitto has all of them.

- **Topics** (`goath1.acl`): `hceu/3/+` (signalling and parties),
  `hceu/3/f/+` (friends), `hceu/3/lobby/s/+` (the listings) and
  `hceu/3/lobby/q` (the browsers' queries). Nothing else can be published
  or subscribed to, `#` and `$SYS` included.
- **Sizes**: the largest packet the game sends is about 620 bytes (a sealed
  512-byte message, its 64-byte topic and the headers); `max_packet_size`
  is 2048. Each client may have 20 messages in flight and 100 (64 KB)
  queued; the broker uses at most 768 MB.
- **Persistence**: the retained listings are saved every minute and on
  shutdown, with their expiry, which the broker honours after a restart.
- **Connections**: 4000 at most; per source address, 30 new a minute (60 at
  once) and 64 open (`nftables-goath1.nft`). Players behind one carrier's
  NAT share an address; each game holds one connection.

Check it from anywhere with Mosquitto's clients:

```sh
mosquitto_sub -h net.goath1.com -V mqttv5 -t 'hceu/3/lobby/s/+' -v -W 5   # the listings now (sealed)
mosquitto_sub -h net.goath1.com -t '#' -W 3                                # refused by the ACL: nothing
```

On the server: `systemctl status mosquitto`, `/var/log/mosquitto/mosquitto.log`.

## The relay

`halo --relay` from OpenH1's Linux release (`OpenH1-Linux.zip`, CI's release
build), in `/opt/openh1-relay/<tag>`, with `/opt/openh1-relay/current`
pointing at the one in use. It runs as `openh1-relay` (no shell, no home but
`/var/lib/openh1-relay`), restarts on failure, and logs its counts each
minute they change: `journalctl -u openh1-relay`. Its limits are the
defaults (`server.relay_pairs` 64 pairs, `server.relay_upload` 8 MB/s): set
others in `/var/lib/openh1-relay/config.toml`.

**Updates.** `openh1-relay-update.timer` runs `update-relay.sh` on Tuesdays
at 09:00 UTC: it downloads the latest release's `SHA256SUMS`, its signature
and `OpenH1-Linux.zip`, checks the signature with the release key
(`port/assets/release-key.pub`, as the game's self-updater does) and the
zip's sum, installs it beside the others and restarts the relay if the
build changed (pairs being relayed then bind again within seconds). The
three newest builds are kept. By hand:

```sh
ssh root@goath1-net-ash update-relay.sh            # the latest, if it is new
ssh root@goath1-net-ash update-relay.sh --force    # again
# back to an older build:
ssh root@goath1-net-ash 'ln -sfn /opt/openh1-relay/build-2077 /opt/openh1-relay/current && systemctl restart openh1-relay'
```

The relay's protocol is versioned on its own (`docs/dedicated_server.md`,
"Relays"), so a relay of an older release keeps serving newer games; an
update is needed only when that version changes.

## Rebuilding the server from scratch

1. Create the server (Ubuntu 24.04, the maintainers' ssh key), its firewall
   and its DNS (port 22 is open only until Tailscale is up):

   ```sh
   hcloud context use goath1
   hcloud server create --name goath1-net-ash --type cpx11 --location ash --image ubuntu-24.04 --ssh-key <key>
   hcloud firewall create --name goath1-net
   hcloud firewall add-rule goath1-net --direction in --protocol tcp --port 22 --source-ips 0.0.0.0/0 --source-ips ::/0 --description ssh-until-tailscale
   hcloud firewall add-rule goath1-net --direction in --protocol tcp --port 1883 --source-ips 0.0.0.0/0 --source-ips ::/0 --description mqtt
   hcloud firewall add-rule goath1-net --direction in --protocol udp --port 2302 --source-ips 0.0.0.0/0 --source-ips ::/0 --description relay
   hcloud firewall add-rule goath1-net --direction in --protocol icmp --source-ips 0.0.0.0/0 --source-ips ::/0 --description ping
   hcloud firewall apply-to-resource goath1-net --type server --server goath1-net-ash
   ```

   and point `net.goath1.com`'s A and AAAA records at its addresses (DNS
   only).
2. Put it on the tailnet (`curl -fsSL https://tailscale.com/install.sh | sh`,
   `tailscale up --hostname goath1-net-ash`), check `ssh
   root@goath1-net-ash` works, then close 22 to the internet: `hcloud
   firewall delete-rule goath1-net --direction in --protocol tcp --port 22
   --source-ips 0.0.0.0/0 --source-ips ::/0 --description
   ssh-until-tailscale`.
3. Copy this folder over and run the setup:

   ```sh
   scp -r deploy/net root@goath1-net-ash:/root/goath1-net
   ssh root@goath1-net-ash sh /root/goath1-net/setup.sh
   ```

   It installs the packages (Mosquitto, nftables, unattended-upgrades), ssh's
   and the updates' settings where they are missing, a 1 GB swap file, the
   files above, and the latest
   release as the relay, and starts everything. It can be run again after
   changing a file here: it puts the files back and restarts what they
   belong to. The nftables rules are loaded last, with a three-minute
   rollback: from a new ssh connection over Tailscale, run `systemctl stop
   goath1-nft-rollback.timer` to keep them. A mistake in them cannot lock
   the server out.
4. Check: `ssh root@goath1-net-ash 'systemctl is-active mosquitto
   openh1-relay nftables; journalctl -u openh1-relay -n 3'`.

The server holds no secrets but its ssh host keys; nothing here needs any.
