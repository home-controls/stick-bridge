# Security

Found a security problem in this project? Please report it privately: on GitHub, open the
**Security** tab of this repository and choose **Report a vulnerability**. Please don't open a public
issue for it.

You'll get an answer as soon as possible, and a fixed release with a note in the changelog once it is
fixed.

## What the bridge protects, and what it doesn't

- **The dashboard and API are plain HTTP** on your network: the password at login, the session cookie and Basic
  credentials can be read by anything that can see the traffic. Keep the bridge on a network you trust (or a VLAN
  of its own).
- **A radio's TCP port gives whoever connects the stick itself**, including its network key. Set each radio's
  allowed host (Settings, Radios) to the one machine that uses it; the bridge warns in its events while a radio
  takes connections from anyone. A saved rule it can't read shuts the radio rather than opening it.
- **The first-boot password** can only be set with the bridge opened by an IP address or by its own hostname
  (`<hostname>` or `<hostname>.local`, so another site's name pointed at it is refused), and only in the first
  30 minutes after it starts.
- **Wrong passwords** lock out the address they come from (5 minutes, doubling up to 2 hours), not everyone; over 100
  in an hour from many addresses make every login wait 15 minutes (signed-in browsers carry on).
- **The API token** (for Zigbee Studio) can read everything, reboot, reset counters, check for updates and set
  radio 1's allowed host to one address or a range of /8 or narrower. It can't install firmware, flash sticks,
  change other settings, the password or itself.
- **Updates** come over HTTPS with a SHA-256 and MD5 from the release's manifest, are read back before use, and roll
  back if the new firmware doesn't start. They aren't signed yet.
