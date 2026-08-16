# Development Test Scripts

These scripts are kept on the `dev` branch for experimental FLRC throughput and video-transfer work.

- `lora_flrc_pair_test.sh`: assisted two-device FLRC throughput test.
- `lora_flrc_video_transfer.sh`: FLRC file/video transfer probe.
- `lora_flrc_verified_probe.sh`: verified FLRC parameter probe.
- `meshtastic_pair_test.sh`: two-device Meshtastic probe smoke test over SSH; add `--ack` to include one direct-message ACK check.
- `meshtastic_official_cli_matrix.sh`: official-device interoperability matrix using the Meshtastic Python CLI and two already-running K230 daemons.

They are not part of the stable `main` branch release flow.
