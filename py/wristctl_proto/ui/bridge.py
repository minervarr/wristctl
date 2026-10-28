"""Run asyncio in a worker thread and marshal results back to the GTK loop."""

from __future__ import annotations

import asyncio
import threading
from typing import Any, Coroutine


class AsyncRunner:
    def __init__(self) -> None:
        self.loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run, name="wristctl-async", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def submit(self, coro: Coroutine[Any, Any, Any]) -> asyncio.Future:
        return asyncio.run_coroutine_threadsafe(coro, self.loop)  # type: ignore[return-value]

    def stop(self) -> None:
        if self.loop.is_running():
            self.loop.call_soon_threadsafe(self.loop.stop)
