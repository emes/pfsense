<?php
/*
 * wg.inc
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2021 Rubicon Communications, LLC (Netgate)
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

require_once("globals.inc");
require_once('config.inc');

$wgbinpath = '/usr/local/bin/wg';

global $wgpeerdefaultroute;
$wgpeerdefaultroute = array();

// Gnerate a preshared key
function gerneratePSK() {
	global $wgbinpath;

	return exec("{$wgbinpath} genpsk");
}

// Return the next available WireGuard port
function next_wg_port() {
	global $config;

	init_config_arr(array('wireguard', 'tunnel'));
	$tunnels = &$config['wireguard']['tunnel'];

	for ($idx=51820; $idx<65535; $idx++) {
		// Check to see if the port is already in use
		$found = false;
		foreach ($tunnels as $tunnel) {
			if ($tunnel['interface']['listenport'] == $idx) {
				$found = true;
				break;
			}
		}

		// If not, it can be used
		if (!$found) {
			return $idx;
		}
	}

	return 51820;
}

// Return the next available WireGuard interface
function next_wg_if() {
	global $config;
	init_config_arr(array('wireguard', 'tunnel'));
	$tunnels = &$config['wireguard']['tunnel'];

	$used_ifs = array();
	foreach ($tunnels as $tunnel) {
		$used_ifs[] = $tunnel['name'];
	}

	for ($ifnum = 0; $ifnum < 32768; $ifnum++) {
		$want_if = "wg{$ifnum}";
		if (!in_array($want_if, $used_ifs)) {
			return $want_if;
		}
	}
	return -1;
}

// Validate the user's input and return error messages if not acceptable
function wg_validate_post($pconfig) {
	$input_errors = array();

	// Check the addresses
	$addrs = explode(",", $pconfig['interface']['address']);

	foreach ($addrs as $addr) {
		$addr = trim($addr);

		if (!is_subnet($addr)) {
			$input_errors[] = sprintf(gettext(
			    '%1$s is not a valid CIDR address'), $addr);
		}

		$iflist = get_configured_interface_list_by_realif();

		$skip = '';
		if (!empty($iflist[$pconfig['name']])) {
			if (!isset($pconfig['enabled']) ||
			    ($pconfig['enabled'] != 'yes')) {
				$input_errors[] = gettext('Cannot disable a WireGuard instance while it is assigned as an interface.');
			}
			$skip = $iflist[$pconfig['name']];
		}

		$a = explode("/", $addr);
		$conflicts = where_is_ipaddr_configured($a[0], $skip, true,
		    true, $a[1]);

		if (!empty($conflicts)) {
			foreach ($conflicts as $conflict) {
				$input_errors[] = sprintf(gettext(
				    '%1$s is already configured on this ' .
				    'firewall: %2$s (%3$s)'), $addr,
					strtoupper($conflict['if']),
					$conflict['ip_or_subnet']);
			}
		}
	}

	// Check listen port
	$lport = $pconfig['interface']['listenport'];
	if (!empty($lport) && (!ctype_digit($lport) || !is_port($lport))) {
		$input_errors[] = gettext("Invalid interface listen port.");
	}

	// Check keys
	if (empty($pconfig['interface']['privatekey'])) {
		$input_errors[] = gettext("Private key must be specified");
	}

	// Now the peers
	if (isset($pconfig['peers']['wgpeer'])) {
		$idx = 0;
		foreach ($pconfig['peers']['wgpeer'] as $peer) {
			$input_errors = array_merge($input_errors,
			    wg_validate_peer($idx, $peer));
			$idx++;
		}
	}

	return $input_errors;
}

// Valildate a peer
function wg_validate_peer($idx, $peer) {
	global $wgpeerdefaultroute;

	$input_errors = array();

	// Check remote port
	$rport = $peer['port'];
	if (!empty($rport) && (!ctype_digit($rport) || !is_port($rport))) {
		$input_errors[] = sprintf(gettext(
		    'Peer %1$s: Invalid remote port. (%2$s)'), $idx, $rport);
	}

	// Check key
	if (empty($peer['publickey'])) {
		$input_errors[] = sprintf(gettext(
		    'Peer %1$s: A public key must be specified'), $idx);
	}

	// Endpoint
	$ep = trim($peer['endpoint']);
	if (!empty($ep) && !is_hostname($ep) && !is_ipaddr($ep)) {
		$input_errors[] = sprintf(gettext(
		    'Peer %1$s: Endpoint must be a valid IPv4 or IPv6 ' .
		    'adress or hostname.'), $idx);
	}

	// Allowed IPs
	if (!empty($peer['allowedips'])) {
		foreach (explode(",", $peer['allowedips']) as $ip) {
			if (!is_subnet(trim($ip))) {
				$input_errors[] = sprintf(gettext(
				    'Peer %1$s: Address %2$s is not a valid ' .
				    'IPv4 or IPv6 CIDR subnet address.'),
					$idx, $ip);
			}
		}
	}

	// Default route, see https://redmine.pfsense.org/issues/11465
	$defroute = wg_defaultroute_check($peer);
	if ($defroute) { 
		if ((($defroute == 'all') && !empty($wgpeerdefaultroute)) ||
		    (($defroute == 'ipv4') && in_array('ipv4', $wgpeerdefaultroute)) ||
		    (($defroute == 'ipv6') && in_array('ipv6', $wgpeerdefaultroute))) {
			$input_errors[] = sprintf(gettext(
			    'Peer %1$s: Only one endpoint per address family can be the default route ' .
			    '(0.0.0.0/0, ::/0, or blank in the Allowed IPs field).'), $idx);
		} else {
			$wgpeerdefaultroute[] = $defroute;
		}
	}

	return $input_errors;
}

// Setup all WireGuard tunnels
function wg_configure() {
	global $config;

	if (!is_array($config["wireguard"]["tunnel"])) {
		return;
	}

	if (platform_booting()) {
		echo gettext("Configuring WireGuard Tunnels... ");
	}

	wg_create_config_files();

	foreach ($config["wireguard"]["tunnel"] as $tunnel) {
		if (isset($tunnel['enabled']) && $tunnel['enabled'] == 'yes') {
			wg_configure_if($tunnel['name']);
		} else {
			wg_destroy_if($tunnel['name']);
		}
	}

	if (platform_booting()) {
		echo gettext("done.") . "\n";
	}
}

// Setup WireGuard tunnel
function wg_configure_if($wg_ifname) {
	global $config, $g;

	if (!is_array($config["wireguard"]["tunnel"])) {
		return;
	}

	unset($tunnel);
	foreach ($config["wireguard"]["tunnel"] as $tun) {
		if (isset($tun['enabled']) && $tun['enabled'] == 'yes' &&
		    $tun['name'] == $wg_ifname) {
			$tunnel = $tun;
			break;
		}
	}

	if (!is_array($tunnel)) {
		return;
	}

	wg_destroy_if($tunnel['name']);
	//$if = pfSense_interface_create($tunnel['name']);
	$conf_path = $g["wg_conf_path"] . "/" . $tunnel['name'] . ".conf";
	/* XXX - workaround while pfSense-module is fixed */
	mwexec("/sbin/ifconfig " . escapeshellarg($tunnel['name']) . " create");
	mwexec("/usr/local/bin/wg setconf " . escapeshellarg($tunnel['name']) .
	    " " . escapeshellarg($conf_path));
	$ip4_first = true;
	$ip6_first = true;
	foreach (explode(",", $tunnel['interface']['address']) as $addr) {
		if (strstr($addr, "/") == false) {
			continue;
		}
		list($ip, $mask) = explode("/", trim($addr));
		if (is_ipaddrv4($ip)) {
			mwexec("/sbin/ifconfig " .
			    escapeshellarg($tunnel['name']) . " inet " .
			    escapeshellarg($ip) . " netmask " .
			    escapeshellarg(gen_subnet_mask($mask)) .
			    ($ip4_first ? "" : " alias"));
			if ($ip4_first) {
				file_put_contents("/tmp/{$tunnel['name']}_router", wg_find_tunnel_gw($tunnel, $addr));
			}
			$ip4_first = false;
		} elseif (is_ipaddrv6($ip)) {
			mwexec("/sbin/ifconfig " .
			    escapeshellarg($tunnel['name']) . " inet6 " .
			    escapeshellarg($ip) . " prefixlen " .
			    escapeshellarg($mask) .
			    ($ip6_first ? "" : " alias"));
			if ($ip6_first) {
				file_put_contents("/tmp/{$tunnel['name']}_routerv6", wg_find_tunnel_gw($tunnel, $addr));
			}
			$ip6_first = false;
		}
	}

	system_routing_configure(
	    convert_real_interface_to_friendly_interface_name($wg_ifname));

	if (is_array($tunnel['peers']) && is_array($tunnel['peers']['wgpeer']) &&
	    count($tunnel['peers']['wgpeer']) > 0) {
		foreach ($tunnel['peers']['wgpeer'] as $peer) {
			if (!empty($peer['allowedips'])) {
				foreach (explode(",", $peer['allowedips']) as $ip) {
					$ip = trim($ip);
					if (empty($ip) ||
					    ($ip == '0.0.0.0/0') ||
					    ($ip == '::/0')) {
						continue;
					}
					route_add_or_change($ip, $tunnel['name'], $tunnel['name']);
				}
			}
		}
	}

	/* set correct MTU on boot/config changes, https://redmine.pfsense.org/issues/11482 */
	$mtu = get_interface_default_mtu('wg');
	if (is_array($config['interfaces'])) {
		foreach ($config['interfaces'] as $tmpinterface) {
			if (($tmpinterface['if'] == $tunnel['name']) && !empty($tmpinterface['mtu'])) {
				$mtu = $tmpinterface['mtu'];
				break;
			}
		}
	}
	set_interface_mtu($tunnel['name'], $mtu);
}

// Remove WireGuard tunnel
function wg_destroy_if($wg_ifname) {
	return (pfSense_interface_destroy($wg_ifname));
}

// Generate private key
function genKeyPair($json = false) {
	global $wgbinpath;

	$privkey = exec("{$wgbinpath} genkey");
	$pubkey = genPubKey($privkey);

	$res = array('privkey' => $privkey, 'pubkey' => $pubkey);
	return $json ? json_encode($res) : $res;
}

// Compose the public key from a provided private key
function genPubKey($privkey) {
	global $wgbinpath;

	return (exec("echo {$privkey} | {$wgbinpath} pubkey"));
}

function deleteTunnel($tunidx) {
	global $config, $g;

	if ($config['wireguard']['tunnel'][$tunidx]) {
		unset($ifname);
		unset($conf_path);
		if (isset($config['wireguard']['tunnel'][$tunidx]['name'])) {
			$ifname =
			    $config['wireguard']['tunnel'][$tunidx]['name'];
		}
		if (isset($ifname)) {
			$conf_path = $g["wg_conf_path"] . "/" . $ifname .
			    ".conf";
		}
		// Delete the tunnel configuration entry
		unset($config['wireguard']['tunnel'][$tunidx]);
		write_config("WireGuard tunnel {$index} updated.");

		// Delete the wg?.conf file
		if (isset($conf_path) && is_file($conf_path)) {
			unlink($conf_path);
		}

		// Destroy the deleted tunnel
		wg_destroy_if($ifname);
	}
}

// Write new tunnel values to the configuration system
function wg_do_post($post, $json = false) {
	global $config;

	init_config_arr(array('wireguard', 'tunnel'));

	$input_errors = array();

	$index = $post['index'];

	$pconfig = &$config['wireguard']['tunnel'][$index];

	if (empty($pconfig['name'])) {
		$pconfig['name'] = next_wg_if();
	}
	$pconfig['enabled'] = empty($post['enabled']) ? 'no':'yes';
	$pconfig['descr'] = $post['descr'];

	// Interface section
	$pconfig['interface']['address'] = $post['address'];
	$pconfig['interface']['listenport'] = $post['listenport'];
	$pconfig['interface']['privatekey'] = $post['privatekey'];
	$pconfig['interface']['publickey'] = genPubKey($post['privatekey']);

	if (isset($post['endpoint0'])) {
		// Peers section
		$pconfig['peers'] = array();
		$pconfig['peers']['wgpeer'] = array();

		$idx = 0;

		for (;;) {
			if (!$post['publickeyp'.$idx] &&
			    !$post['endpoint'.$idx] &&
			    !$post['allowedips'.$idx] &&
			    !$post['descr'.$idx] &&
			    !$post['persistentkeepalive'.$idx]) {
				break;
			}

			$peer = array();
			$peer['publickey'] = $post['publickeyp' . $idx];
			$peer['endpoint'] = $post['endpoint' . $idx];
			$peer['allowedips'] = $post['allowedips' . $idx];
			$peer['descr'] = $post['descp' . $idx];
			$peer['persistentkeepalive'] =
			    $post['persistentkeepalive' . $idx];
			$peer['presharedkey'] = $post['presharedkey' . $idx];
			$peer['port'] = $post['port' . $idx];
			$peer['peerwgaddr'] = $post['peerwgaddr' . $idx];

			$pconfig['peers']['wgpeer'][] = $peer;
			$idx++;
		}
	} else {
		unset($pconfig['peers']);
	}

	$input_errors = wg_validate_post($pconfig);

	if (!$input_errors) {
		$config['wireguard']['tunnel'][$index] = $pconfig;
		write_config("WireGuard tunnel {$index} updated.");
	}

	return(array('input_errors' => $input_errors, 'pconfig' => $pconfig));
}

/*
 * Read the WireGuard configurations from config.xml and create a number of
 * *.conf files for wg-quick to read
 */
function wg_create_config_files($clean = true) {
	global $config, $g;

	if ($config["wireguard"]["tunnel"]) {
		$wg_tunnels = $config["wireguard"]["tunnel"];
		$cfgpath = $g["wg_conf_path"];

		if (!file_exists($cfgpath)) {
			mkdir($cfgpath, 0700, true);
		} else {
			chmod($cfgpath, 0700);
		}

		if ($clean) {
			delete_wg_configs();
		}

		foreach ($wg_tunnels as $tunnel) {
			if (!empty($tunnel['enabled']) &&
			    $tunnel['enabled'] == 'yes') {
				make_wg_conf($tunnel);
			}
		}
	}
}

// Write each file
function make_wg_conf($tunnel) {
	global $g;

	$txt = "# This WireGuard config file has been created automatically. " .
	    "Do not edit!\n";

	$txt .= "# Description: {$tunnel['descr']}\n\n";

	// Process Interfaces section
	$txt .= "[Interface]\n";

	if (!empty($tunnel["interface"]["privatekey"])) {
		$txt .= "PrivateKey = {$tunnel["interface"]["privatekey"]}\n";
	}

	if (!empty($tunnel["interface"]["listenport"])) {
		$txt .= "ListenPort = {$tunnel["interface"]["listenport"]}\n";
	}

	$txt .= "\n";

	// Process peers section
	if (is_array($tunnel['peers']) && is_array($tunnel['peers']['wgpeer']) &&
	    count($tunnel['peers']['wgpeer']) > 0) {
		foreach ($tunnel['peers']['wgpeer'] as $peer) {
			$txt .= "# Peer: {$peer['descr']}\n";
			$txt .= "[Peer]\n";

			if (!empty($peer["publickey"])) {
				$txt .= "PublicKey = {$peer['publickey']}\n";
			}

			if (!empty($peer["endpoint"])) {
				$txt .= "EndPoint = ";
				$txt .= is_ipaddrv6($peer["endpoint"]) ? "[{$peer['endpoint']}]" : $peer['endpoint'];
				$txt .= ":" . ((empty($peer["port"])) ? '51820' : $peer["port"]) . "\n";
			}

			if (!empty($peer["allowedips"])) {
				$txt .= "AllowedIPs = {$peer['allowedips']}\n";
			}

			if (!empty($peer["persistentkeepalive"])) {
				$txt .= "PersistentKeepalive = " .
				    "{$peer['persistentkeepalive']}\n";
			}

			if (!empty($peer["presharedkey"])) {
				$txt .= "PresharedKey = " .
				    "{$peer['presharedkey']}\n";
			}

			$txt .= "\n";
		}
	}

	file_put_contents($g["wg_conf_path"] . "/" . $tunnel['name'] . ".conf",
	    $txt);
	chmod($g["wg_conf_path"] . "/" . $tunnel['name'] . ".conf", 0600);
}

// Remove all wg config files from the conf directory
function delete_wg_configs() {
	global $g;

	unlink_if_exists($g["wg_conf_path"] . "/*.conf");
}

/* Check if at least one tunnel is enabled */
function is_wg_enabled() {
	global $config;

	init_config_arr(array('wireguard', 'tunnel'));

	foreach ($config['wireguard']['tunnel'] as $tunnel) {
		if (empty($tunnel['enabled'])) {
			continue;
		}
		return true;
	}

	return false;
}

/* Return WireGuard tunnel networks for a given address family */
function wg_get_tunnel_networks($family = 'both') {
	global $config;
	$wg_tunnel_networks = array();
	init_config_arr(array('wireguard', 'tunnel'));
	if (is_wg_enabled()) {
		foreach ($config['wireguard']['tunnel'] as $wg) {
			if (empty($wg['enabled']) ||
			    empty($wg['interface']['address'])) {
				continue;
			}
			foreach(explode(',', $wg['interface']['address']) as $wga) {
				list($wgnet, $wgmask) = explode('/', trim($wga));
				if ((is_ipaddrv6($wgnet) && ($family == 'ipv4')) ||
				    (is_ipaddrv4($wgnet) && ($family == 'ipv6'))) {
					continue;
				}
				$network = gen_subnet($wgnet, $wgmask);
				$wg_tunnel_networks[] = "{$network}/{$wgmask}";
			}
		}
	}
	return $wg_tunnel_networks;
}

/* Locate a viable remote gateway address for a WireGuard tunnel
 * Fall back to using the tunnel address itself.
 * https://redmine.pfsense.org/issues/11300 */
function wg_find_tunnel_gw($tunnel, $addr) {
	list($ip, $mask) = explode("/", trim($addr));
	/* Loop through peers looking for a viable remote gateway address */
	if (is_array($tunnel['peers']) &&
	    is_array($tunnel['peers']['wgpeer']) &&
	    count($tunnel['peers']['wgpeer']) > 0) {
		foreach ($tunnel['peers']['wgpeer'] as $peer) {
			/* If this peer has no configured Peer WireGuard Address, skip it. */
			if (empty($peer['peerwgaddr'])) {
				continue;
			}
			/* Check each Peer WireGuard Address entry */
			foreach (explode(',', $peer['peerwgaddr']) as $pwga) {
				/* Ensure the address family of this entry matches the one we're seeking */
				if (is_v4($ip) !== is_v4($pwga)) {
					continue;
				}
				/* If there is a subnet mask, ditch it. */
				list($pip, $pmask) = explode('/', trim($pwga));
				/* Check that this address is in the desired subnet */
				if (ip_in_subnet($pip, trim($addr))) {
					/* We found a good candidate, return it */
					return $pip;
				}
			}
		}
	}
	/* If no viable candidate is found, return the tunnel address */
	return $ip;
}

function wg_defaultroute_check($peer) {

	if (empty($peer['allowedips'])) {
		return 'all';
	} else {
		foreach (explode(",", $peer['allowedips']) as $ip) {
			$ip = trim($ip);
			if ($ip == '0.0.0.0/0') {
				return 'ipv4';
			} elseif ($ip == '::/0') {
				return 'ipv6';
			}
		}
	}
	return false;
}
