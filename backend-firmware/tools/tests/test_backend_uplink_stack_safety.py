from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]
UPLINK = REPO_ROOT / "backend-firmware/uplink"
MIN_HEADROOM_BYTES = 2048
TASK_STACK_BYTES = {
    "uplink-s3-backend": 14336,
    "uplink-s3-fullsize-backend": 14336,
}
CALL_CHAINS = {
    "coordinator_upload": (7168, ("coordinator_worker", "queue_upload_locked")),
    "uploader_upload": (14336, (
        "uploader_worker", "backend_http_post_json", "perform_request",
    )),
    "command_result": (14336, (
        "command_worker", "command_send_result", "backend_http_post_json",
        "perform_request",
    )),
    "command_poll": (14336, (
        "command_worker", "backend_http_get_json", "perform_request",
    )),
}


@pytest.mark.parametrize("environment", tuple(TASK_STACK_BYTES))
def test_embedded_uplink_stack_usage_keeps_large_transport_buffers_off_tasks(
    environment: str,
):
    usage_files = list((UPLINK / ".pio/build" / environment).rglob("*.su"))
    assert usage_files, "build with -fstack-usage before running this check"
    text = "\n".join(path.read_text(encoding="utf-8", errors="replace")
                      for path in usage_files)
    usage_by_function = {}
    for function in {name for _budget, chain in CALL_CHAINS.values()
                     for name in chain}:
        matching = [line for line in text.splitlines() if function in line]
        assert matching, f"missing stack-usage entry for {function}"
        usage_by_function[function] = max(
            int(line.split("\t")[1]) for line in matching
        )
    for name, (budget, chain) in CALL_CHAINS.items():
        usage = sum(usage_by_function[function] for function in chain)
        assert usage <= budget - MIN_HEADROOM_BYTES, (
            f"{name} uses {usage} bytes, leaving {budget - usage} bytes"
        )
