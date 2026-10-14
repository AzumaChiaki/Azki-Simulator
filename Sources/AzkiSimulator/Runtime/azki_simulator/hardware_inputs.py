"""Validated, bounded batches for the existing guest touch path only.

Validation is atomic; hardware execution is not transactional. A partial send
must remain partial in the receipt. No ADC, optical or account data is invented.
"""
from __future__ import annotations

import time


class HardwareInputError(ValueError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


def validate_touch_batch(value, *, geometry, generation, previous_sequence):
    if not isinstance(value, dict) or set(value) != {"kind", "generation", "sequence", "events"}:
        raise HardwareInputError("invalid_batch", "Expected a touch batch with generation, sequence and events")
    if value["kind"] != "touch":
        raise HardwareInputError("unsupported_hardware", "This backend currently consumes touch input only")
    if type(value["generation"]) is not int or value["generation"] != generation:
        raise HardwareInputError("stale_generation", "Batch belongs to a different core start")
    sequence = value["sequence"]
    if type(sequence) is not int or not previous_sequence < sequence <= 2**53 - 1:
        raise HardwareInputError("stale_sequence", "Batch sequence must increase within the core generation")
    events = value["events"]
    if not isinstance(events, list) or not 1 <= len(events) <= 32:
        raise HardwareInputError("invalid_batch", "Touch batch requires 1…32 events")
    width, height = geometry
    prior_time = -1
    copied = []
    for event in events:
        if not isinstance(event, dict) or set(event) != {"timestampMs", "x", "y", "down"}:
            raise HardwareInputError("invalid_batch", "Unexpected touch event schema")
        when, x, y, down = (event[name] for name in ("timestampMs", "x", "y", "down"))
        if (type(when) is not int or not 0 <= when <= 1500 or when < prior_time or
                type(x) is not int or not 0 <= x < width or type(y) is not int or
                not 0 <= y < height or type(down) is not bool):
            raise HardwareInputError("invalid_batch", "Invalid device pixels, timestamp or contact state")
        copied.append(dict(event))
        prior_time = when
    if copied[0]["timestampMs"] != 0 or copied[-1]["down"]:
        raise HardwareInputError("invalid_batch", "Batch starts at 0 ms and ends with a contact release")
    return sequence, tuple(copied)


class TouchBatchExecutor:
    def __init__(self):
        self.generation = 0
        self.sequence = 0

    def new_generation(self, generation):
        self.generation, self.sequence = generation, 0

    def execute(self, batch, *, geometry, send, cancelled=lambda: False):
        sequence, events = validate_touch_batch(batch, geometry=geometry,
            generation=self.generation, previous_sequence=self.sequence)
        # Reserve after complete validation, before any external effect. A
        # cancelled/partially sent request cannot accidentally be replayed.
        self.sequence = sequence
        start = time.monotonic()
        applied, error, last = 0, None, events[0]
        release = False
        try:
            for event in events:
                due = start + event["timestampMs"] / 1000
                while time.monotonic() < due:
                    if cancelled():
                        raise HardwareInputError("cancelled", "Touch batch was cancelled")
                    time.sleep(min(.01, max(0, due - time.monotonic())))
                if cancelled() or time.monotonic() - start >= 3:
                    raise HardwareInputError("cancelled_or_timeout", "Touch batch stopped before the next event")
                last = event
                response = send({key: event[key] for key in ("x", "y", "down")})
                if not isinstance(response, dict) or response.get("accepted") is not True:
                    raise HardwareInputError("input_rejected", "Backend did not accept the touch event")
                applied += 1
            release = True
        except (OSError, ValueError, KeyError, TypeError) as failure:
            error = getattr(failure, "code", "input_failed")
        finally:
            if not release:
                try:
                    response = send({"x": last["x"], "y": last["y"], "down": False})
                    release = isinstance(response, dict) and response.get("accepted") is True
                except (OSError, ValueError, KeyError, TypeError):
                    pass
        return {"kind": "touch", "generation": self.generation, "sequence": sequence,
                "validationAtomic": True, "executionAtomic": False,
                "sentEvents": applied, "totalEvents": len(events),
                "completed": applied == len(events) and error is None,
                "releaseAccepted": release, "guestAcknowledged": False,
                "guestAcknowledgement": "requires independent guest trace evidence",
                "error": error}
