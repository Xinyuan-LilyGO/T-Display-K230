# Development Test Scripts

These scripts are kept on the `dev` branch for experimental FLRC throughput and video-transfer work.

- `lora_flrc_pair_test.sh`: assisted two-device FLRC throughput test.
- `lora_flrc_video_transfer.sh`: FLRC file/video transfer probe.
- `lora_flrc_verified_probe.sh`: verified FLRC parameter probe.
- `meshtastic_pair_test.sh`: two-device Meshtastic probe smoke test over SSH; add `--ack` to include one direct-message ACK check.
- `meshtastic_official_cli_matrix.sh`: official-device interoperability matrix using the Meshtastic Python CLI and two already-running K230 daemons. It checks official-to-K230 reliable text, telemetry, optional position, traceroute with retry, and optional K230-to-official reliable text plus K230-originated NodeInfo/traceroute requests with retry when `--official-node '!050da224'` is provided. The script stores both K230 daemon log tails for diagnosis.
- `meshtastic_voice_pair_test.sh`: two-device K230 voice-note smoke test over SSH. It sends a short 8 kHz S16_LE mono PCM test note in both directions, then verifies the peer decoded `/tmp/k230_mesh_voice_rx_*.raw`.
- `meshtastic_fixed_position_cli_test.sh`: temporary fixed-position interoperability test for validating official-device position requests without enabling fake GPS as a release default.

They are not part of the stable `main` branch release flow.
