#!/usr/bin/env python3
"""Start every local AI model AIRogue's settings name.

The game never starts inference servers itself: it opens an HTTP client to
whatever the settings point at. For a local Ollama model that means the tag has
to be pulled once, and the model has to be resident before the first NPC or
navigator request; for a local System 1 decision model it means a llama-server
has to be up on the right port. This script does that from the same two files
the game reads, so the two cannot drift apart.

What it reads
-------------
`settings.json` (or `--settings PATH`) and its sibling `settings.local.json`,
merged with the same RFC 7396 merge patch the engine uses. It then resolves the
same `ai.profiles` indirection the loader resolves, and collects:

* every local Ollama model used by an active model block -- the NPC backend,
  the navigator, the designer and rewriter, the vision backend, System 1;
* every local model declared in `ai.profiles` or `system1.profiles`, even if no
  block selects it yet (the settings window can switch to it at runtime);
* a `local_kev` System 1 preset, served by `scripts/run_decision_server.sh` when
  the GGUF is present.

`--active-only` narrows the list to what the current configuration selects.
`--dry-run` prints the list without touching a server.

What it starts
--------------
For Ollama models it makes sure `ollama serve` is reachable (starting it on a
loopback endpoint when it is not), pulls missing tags only with `--pull`, and
loads each model with the config's `keepAlive` (or `--keep-alive`) so it is warm
before the game asks for it. Loading is sequential, because loading several
multi-GB models at once is how a 15 GiB machine starts swapping.

For a `local_kev` preset it starts `scripts/run_decision_server.sh` in the
background with the configured port, waits for `/health`, and leaves the server
running. The log and PID live under `logs/local-models/`.

Exit status: 0 when every selected model is serving (a declared-but-unselected
model that could not start is a warning), 1 when a selected model failed, and 2
for a settings-file problem.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen

REPO_ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = REPO_ROOT / "logs" / "local-models"
DEFAULT_OLLAMA_ENDPOINT = "http://localhost:11434/api/generate"
DEFAULT_OLLAMA_PORT = 11434
DEFAULT_MODEL_SECTION: dict[str, Any] = {
    "type": "ollama",
    "model": "llama3",
    "apiKey": "",
    "endpoint": DEFAULT_OLLAMA_ENDPOINT,
    "keepAlive": "",
}
DEFAULT_VISION_KEEP_ALIVE = "30m"
LOCAL_KEV_ENDPOINT = "http://127.0.0.1:9931/v1/systemone"
LOCAL_KEV_MODEL = REPO_ROOT / "models" / "Kev-4B-Q4_K_M.gguf"
HOSTED_DECISIONS_ENDPOINT = "https://openrouter.ai/api/alpha/decisions"
HOSTED_DECISIONS_MODEL = "~typesafe/jev-latest"


# ---------------------------------------------------------------------------
# Settings loading
# ---------------------------------------------------------------------------


def merge_patch(base: Any, patch: Any) -> Any:
    """RFC 7396 merge patch, matching nlohmann::json::merge_patch in the loader.

    A null in the patch deletes the key; nested objects merge key by key. The
    override layer relies on this, so `"profile": null` means "remove it".
    """
    if not isinstance(patch, dict):
        return patch
    result = dict(base) if isinstance(base, dict) else {}
    for key, value in patch.items():
        if value is None:
            result.pop(key, None)
        else:
            result[key] = merge_patch(result.get(key), value)
    return result


def as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def text(value: Any, default: str = "") -> str:
    return value if isinstance(value, str) else default


def truth(value: Any, default: bool) -> bool:
    return value if isinstance(value, bool) else default


def integer(value: Any, default: int) -> int:
    if isinstance(value, bool):
        return default
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    return default


def settings_error(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def load_settings(path: Path) -> tuple[dict[str, Any], list[Path]]:
    """Load the tracked file and merge the untracked override on top."""
    used: list[Path] = []
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        settings_error(
            f"no settings file at {path}\n"
            "       Run from the game's working directory, or pass --settings PATH."
        )
    except json.JSONDecodeError as exc:
        settings_error(f"could not parse {path}: {exc}")
    if not isinstance(root, dict):
        settings_error(f"{path} does not contain a JSON object")
    used.append(path)

    local_path = path.parent / "settings.local.json"
    if local_path.exists():
        try:
            local = json.loads(local_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            settings_error(f"could not parse {local_path}: {exc}")
        if not isinstance(local, dict):
            settings_error(f"{local_path} does not contain a JSON object")
        root = merge_patch(root, local)
        used.append(local_path)

    return root, used


# ---------------------------------------------------------------------------
# Profile and endpoint resolution
# ---------------------------------------------------------------------------


@dataclass
class Profile:
    id: str
    label: str
    kind: str
    preset: str
    model: dict[str, Any]


def load_profiles(root: dict[str, Any]) -> dict[str, Profile]:
    """Read ai.profiles with the same defaults the C++ loader gives each entry."""
    profiles: dict[str, Profile] = {}
    raw = as_dict(as_dict(root.get("ai")).get("profiles"))
    for profile_id, entry in raw.items():
        if not isinstance(entry, dict):
            continue
        kind = text(entry.get("kind"), "s2").lower()
        preset = text(entry.get("preset"), "")
        model = dict(DEFAULT_MODEL_SECTION)
        # A preset is the authority for how to reach an S1 model, so the C++
        # loader clears the inherited chat-backend endpoint/model first. Do the
        # same, or a `local_kev` profile would look like an Ollama model.
        if preset and kind == "s1":
            model["model"] = ""
            model["endpoint"] = ""
            model["apiKey"] = ""
        overlay_model(model, as_dict(entry.get("model")))
        model["type"] = model["type"].lower()
        profiles[profile_id] = Profile(
            id=profile_id,
            label=text(entry.get("label"), profile_id),
            kind=kind,
            preset=preset,
            model=model,
        )
    return profiles


def overlay_model(base: dict[str, Any], values: dict[str, Any]) -> dict[str, Any]:
    """Apply a model-shaped JSON block over a resolved model section."""
    for key in ("type", "model", "apiKey", "endpoint", "keepAlive"):
        if isinstance(values.get(key), str):
            base[key] = values[key]
    return base


def resolve_model_block(
    block: dict[str, Any], profiles: dict[str, Profile]
) -> dict[str, Any]:
    """Resolve `profile` first, then let the block's own fields override it.

    This mirrors GameECSInit's BlockSeed pass plus the scalar pass: a block that
    names a profile starts from the profile's whole model section, and a field
    written inline wins.
    """
    profile_id = text(block.get("profile"), "")
    profile = profiles.get(profile_id)
    model = dict(profile.model) if profile is not None else dict(DEFAULT_MODEL_SECTION)
    overlay_model(model, block)
    model["type"] = text(model.get("type"), "ollama").lower()
    return model


def endpoint_base(endpoint: str) -> str:
    """The scheme://host:port part of an endpoint, without a trailing slash."""
    value = endpoint.strip() or DEFAULT_OLLAMA_ENDPOINT
    if "://" not in value:
        value = "http://" + value
    parsed = urlparse(value)
    netloc = parsed.netloc or parsed.path.split("/", 1)[0]
    return f"{parsed.scheme or 'http'}://{netloc}".rstrip("/")


def endpoint_host(endpoint: str) -> str:
    host = urlparse(endpoint_base(endpoint)).hostname or ""
    return host.strip("[]").lower()


def endpoint_port(endpoint: str, default: int) -> int:
    parsed = urlparse(endpoint_base(endpoint))
    return parsed.port or default


def is_local_host(host: str) -> bool:
    if host in {"localhost", "0.0.0.0", "::"}:
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def is_loopback_host(host: str) -> bool:
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def is_local_endpoint(endpoint: str) -> bool:
    host = endpoint_host(endpoint)
    return bool(host) and is_local_host(host)


def normalize_tag(model: str) -> str:
    """Make `qwen2.5vl` and `qwen2.5vl:latest` compare equal."""
    name = model.strip()
    if not name:
        return name
    last = name.rsplit("/", 1)[-1]
    return name if ":" in last else name + ":latest"


def ollama_generate_url(endpoint: str) -> str:
    base = endpoint_base(endpoint)
    parsed = urlparse(endpoint if "://" in endpoint else "http://" + endpoint)
    if parsed.path.rstrip("/").endswith("/api/generate"):
        return f"{base}{parsed.path}"
    prefix = parsed.path.rstrip("/")
    if prefix and not prefix.endswith("/api"):
        return f"{base}{prefix}/api/generate"
    return f"{base}/api/generate"


# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------


@dataclass
class Target:
    """One local backend the config points at."""

    kind: str  # "ollama", "llama_server", "external"
    model: str
    endpoint: str
    sources: list[str] = field(default_factory=list)
    active: bool = False
    keep_alive: str = ""
    options: dict[str, Any] = field(default_factory=dict)
    model_path: Path | None = None
    conflict: str = ""


class Collector:
    def __init__(self, default_keep_alive: str):
        self.default_keep_alive = default_keep_alive
        self.targets: dict[tuple[str, str, str], Target] = {}
        self.notes: list[str] = []

    def _add(
        self,
        target: Target,
        source: str,
        active: bool,
        keep_alive: str = "",
        options: dict[str, Any] | None = None,
    ) -> None:
        key = (target.kind, endpoint_base(target.endpoint), normalize_tag(target.model))
        existing = self.targets.get(key)
        if existing is None:
            target.sources = [source]
            target.active = active
            target.keep_alive = keep_alive or self.default_keep_alive
            target.options = dict(options or {})
            self.targets[key] = target
            return

        existing.sources.append(source)
        existing.active = existing.active or active
        if keep_alive and keep_alive_preference(keep_alive, existing.keep_alive):
            existing.keep_alive = keep_alive
        for name, value in (options or {}).items():
            # A model can be referenced by several blocks with different context
            # windows. Loading it with the largest one avoids a reload when the
            # widest caller arrives.
            if name == "num_ctx" and isinstance(value, int):
                existing.options[name] = max(integer(existing.options.get(name), 0), value)
            else:
                existing.options.setdefault(name, value)

    def add_ollama(
        self,
        model: str,
        endpoint: str,
        source: str,
        active: bool,
        keep_alive: str = "",
        options: dict[str, Any] | None = None,
    ) -> None:
        if not model or not endpoint:
            return
        if not is_local_endpoint(endpoint):
            self.notes.append(
                f"{source}: {model} is on {endpoint}, which is not local; skipped"
            )
            return
        self._add(
            Target("ollama", model, endpoint),
            source,
            active,
            keep_alive=keep_alive,
            options=options,
        )

    def add_llama_server(
        self,
        endpoint: str,
        model_path: Path | None,
        source: str,
        active: bool,
    ) -> None:
        target = Target("llama_server", "local_kev", endpoint, model_path=model_path)
        key = (target.kind, endpoint_base(endpoint), normalize_tag(target.model))
        existing = self.targets.get(key)
        if existing is None:
            self.targets[key] = target
            target.sources = [source]
            target.active = active
            return
        existing.sources.append(source)
        existing.active = existing.active or active
        if model_path is not None and existing.model_path is not None:
            if model_path != existing.model_path:
                existing.conflict = (
                    f"profiles select different GGUF files for {endpoint}: "
                    f"{existing.model_path} and {model_path}"
                )
        elif existing.model_path is None:
            existing.model_path = model_path

    def add_external(self, endpoint: str, model: str, source: str, active: bool) -> None:
        if not is_local_endpoint(endpoint):
            return
        self._add(Target("external", model or "local", endpoint), source, active)

    def add_model_block(
        self,
        block: dict[str, Any],
        source: str,
        active: bool,
        options: dict[str, Any] | None = None,
    ) -> None:
        """Add a model block if it resolves to a local Ollama backend."""
        backend = text(block.get("type"), "ollama").lower()
        if backend in {"gemini", "openrouter"}:
            return
        endpoint = text(block.get("endpoint"), DEFAULT_OLLAMA_ENDPOINT)
        model = text(block.get("model"), "")
        if not model:
            return
        keep_alive = text(block.get("keepAlive"), "")
        self.add_ollama(
            model,
            endpoint,
            source,
            active,
            keep_alive=keep_alive,
            options=options,
        )


def keep_alive_preference(candidate: str, current: str) -> bool:
    """True when `candidate` keeps a model resident at least as long as `current`."""
    if not current:
        return True
    candidate_seconds = keep_alive_seconds(candidate)
    current_seconds = keep_alive_seconds(current)
    if candidate_seconds is None or current_seconds is None:
        return False
    return candidate_seconds > current_seconds


def keep_alive_seconds(value: str) -> float | None:
    """Parse the Go-style durations Ollama accepts (30m, 1h, 3600, -1)."""
    number = value.strip().lower()
    if not number:
        return 0.0
    if number == "-1":
        return float("inf")
    total = 0.0
    digits = ""
    units = {"ms": 0.001, "s": 1.0, "m": 60.0, "h": 3600.0, "d": 86400.0}
    index = 0
    while index < len(number):
        char = number[index]
        if char.isdigit() or char == ".":
            digits += char
            index += 1
            continue
        unit = "ms" if number[index : index + 2] == "ms" else char
        if not digits or unit not in units:
            return None
        total += float(digits) * units[unit]
        digits = ""
        index += len(unit)
    if digits:
        total += float(digits)
    return total


def collect_targets(
    root: dict[str, Any],
    profiles: dict[str, Profile],
    default_keep_alive: str,
    active_only: bool,
) -> Collector:
    collector = Collector(default_keep_alive)

    # --- the NPC backend ---------------------------------------------------
    main_block = resolve_model_block(as_dict(root.get("model")), profiles)
    collector.add_model_block(main_block, "model", active=True)

    # --- vision ------------------------------------------------------------
    collect_vision(root, profiles, collector)

    # --- the single navigator ---------------------------------------------
    navigation = as_dict(root.get("navigation"))
    agent = text(navigation.get("agent"), "single").lower()
    if agent == "single":
        single = as_dict(navigation.get("single"))
        if not truth(single.get("dedicatedBackend"), True):
            pass  # the navigator shares the NPC backend
        elif truth(single.get("useMainModel"), True):
            pass  # already collected as the main model
        else:
            navigation_block = resolve_model_block(as_dict(single.get("model")), profiles)
            if not text(navigation_block.get("model"), ""):
                # The loader reports this and falls back to the main model.
                navigation_block = dict(main_block)
            context_tokens = integer(single.get("contextTokens"), 4096)
            options = {"num_ctx": context_tokens} if context_tokens > 0 else None
            profile_id = text(as_dict(single.get("model")).get("profile"), "")
            suffix = f" (profile {profile_id})" if profile_id else ""
            collector.add_model_block(
                navigation_block,
                f"navigation.single.model{suffix}",
                active=True,
                options=options,
            )

    # --- designer and rewriter --------------------------------------------
    designer = as_dict(root.get("designer"))
    if not truth(designer.get("useMainModel"), True):
        collector.add_model_block(
            resolve_model_block(as_dict(designer.get("model")), profiles),
            "designer.model",
            active=True,
        )

    rewriter = as_dict(designer.get("rewriter"))
    if truth(rewriter.get("enabled"), False):
        collector.add_model_block(
            resolve_model_block(as_dict(rewriter.get("model")), profiles),
            "designer.rewriter.model",
            active=True,
        )

    # --- System 1 ----------------------------------------------------------
    collect_system1(root, profiles, collector, active_only=active_only)

    if active_only:
        return collector

    # --- declared but not currently selected -----------------------------
    # The settings window can switch any of these at runtime, and the request
    # was to start the models the config names, not only the ones selected this
    # second. They are added after the active ones so the game's own backends
    # are warm first.
    for profile_id, profile in profiles.items():
        source = f"ai.profiles.{profile_id} (declared)"
        preset = profile.preset.lower()
        if preset in {"local_kev", "kev"} and profile.kind == "s1":
            endpoint = text(profile.model.get("endpoint"), "") or LOCAL_KEV_ENDPOINT
            if is_local_endpoint(endpoint):
                collector.add_llama_server(
                    endpoint,
                    resolve_local_model_path(profile.model.get("model")),
                    source,
                    active=False,
                )
        else:
            # An unreferenced profile is only worth starting when it names a
            # model explicitly; an empty profile would just mean the compiled-in
            # llama3 default.
            if text(profile.model.get("model"), ""):
                collector.add_model_block(profile.model, source, active=False)

    return collector


def collect_vision(
    root: dict[str, Any], profiles: dict[str, Profile], collector: Collector
) -> None:
    vision = as_dict(root.get("vision"))
    if not truth(vision.get("enabled"), True):
        return

    profile_id = text(vision.get("profile"), "")
    profile = profiles.get(profile_id)
    if profile is not None and text(profile.model.get("model"), ""):
        model = dict(profile.model)
        source = f"vision.profile {profile_id}"
    else:
        model = {
            "type": "ollama",
            "model": "moondream",
            "endpoint": DEFAULT_OLLAMA_ENDPOINT,
            "keepAlive": DEFAULT_VISION_KEEP_ALIVE,
        }
        source = "vision"

    for key in ("model", "endpoint", "keepAlive"):
        if isinstance(vision.get(key), str):
            model[key] = vision[key]
    model["type"] = "ollama"
    collector.add_model_block(model, source, active=True)


def collect_system1(
    root: dict[str, Any],
    profiles: dict[str, Profile],
    collector: Collector,
    active_only: bool,
) -> None:
    system1 = as_dict(root.get("system1"))
    active_id = text(system1.get("activeProfile"), "")
    entries = [entry for entry in as_list(system1.get("profiles")) if isinstance(entry, dict)]

    chosen = None
    for entry in entries:
        if active_id and text(entry.get("id"), "").lower() == active_id.lower():
            chosen = entry
            break

    if chosen is not None:
        add_system1_entry(chosen, profiles, collector, active=True)
    elif truth(system1.get("useMainModel"), True):
        pass  # System 1 shares the NPC backend
    else:
        add_system1_entry(system1, profiles, collector, active=True, is_flat=True)

    if active_only:
        return

    for entry in entries:
        if entry is chosen:
            continue
        add_system1_entry(entry, profiles, collector, active=False)


def add_system1_entry(
    entry: dict[str, Any],
    profiles: dict[str, Profile],
    collector: Collector,
    active: bool,
    is_flat: bool = False,
) -> None:
    """Resolve one System 1 backend, which is a decisions endpoint, not chat."""
    label = text(entry.get("id"), "default") if not is_flat else "default"
    entry_model = as_dict(entry.get("model"))
    named = profiles.get(text(entry.get("profile"), ""))

    preset = text(entry.get("preset"), "")
    if not preset and named is not None:
        preset = named.preset
    preset = preset.lower()

    endpoint = HOSTED_DECISIONS_ENDPOINT
    model = HOSTED_DECISIONS_MODEL
    if preset in {"local_kev", "kev"}:
        endpoint = LOCAL_KEV_ENDPOINT
        model = "local"
    elif preset not in {"", "hosted_jev", "jev"}:
        # Unknown preset: the engine reports it and uses explicit fields only.
        endpoint = ""

    if named is not None:
        endpoint = text(named.model.get("endpoint"), endpoint) or endpoint
        model = text(named.model.get("model"), model) or model
    endpoint = text(entry_model.get("endpoint"), endpoint) or endpoint
    model = text(entry_model.get("model"), model) or model

    # A chat endpoint is not a decisions endpoint. SystemOneProfiles replaces it
    # with the hosted one, so an explicit /api/generate here is not a local
    # System 1 server.
    if "/api/generate" in endpoint:
        endpoint = HOSTED_DECISIONS_ENDPOINT

    source = f"system1.{'model' if is_flat else 'profiles.' + label}"
    if preset in {"local_kev", "kev"}:
        if is_local_endpoint(endpoint):
            collector.add_llama_server(
                endpoint,
                resolve_local_model_path(model),
                source,
                active=active,
            )
        return

    if is_local_endpoint(endpoint):
        collector.add_external(endpoint, model, source, active)


def resolve_local_model_path(configured: str) -> Path:
    """Where the bundled llama-server's GGUF lives.

    `model` in a local_kev preset is advisory ("local"), so only a value that
    actually names a .gguf is treated as a path. An environment override is
    useful when a different decision model is being tried.
    """
    env_path = os.environ.get("AIROGUE_KEV_MODEL") or os.environ.get("KEV_MODEL")
    if env_path:
        path = Path(env_path)
        return path if path.is_absolute() else (REPO_ROOT / path)
    candidate = configured.strip() if isinstance(configured, str) else ""
    if candidate.lower().endswith(".gguf"):
        path = Path(candidate)
        return path if path.is_absolute() else (REPO_ROOT / path)
    return LOCAL_KEV_MODEL


# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------


def http_json(
    url: str, payload: dict[str, Any] | None = None, timeout: float = 5.0
) -> dict[str, Any]:
    data = None
    headers: dict[str, str] = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = Request(url, data=data, headers=headers)
    with urlopen(request, timeout=timeout) as response:
        body = response.read()
    return json.loads(body) if body else {}


def http_error_message(error: HTTPError) -> str:
    try:
        body = error.read().decode("utf-8", "replace")
    except Exception:
        return f"HTTP {error.code}"
    try:
        parsed = json.loads(body)
        if isinstance(parsed, dict) and parsed.get("error"):
            return str(parsed["error"])
    except json.JSONDecodeError:
        pass
    return body.strip() or f"HTTP {error.code}"


def ollama_version(base: str, timeout: float = 2.0) -> str | None:
    try:
        payload = http_json(f"{base}/api/version", timeout=timeout)
        return text(payload.get("version"), "") or "unknown"
    except (HTTPError, URLError, OSError, json.JSONDecodeError):
        return None


def ollama_tags(base: str) -> dict[str, dict[str, Any]]:
    payload = http_json(f"{base}/api/tags", timeout=10.0)
    result: dict[str, dict[str, Any]] = {}
    for item in as_list(payload.get("models")):
        if not isinstance(item, dict):
            continue
        name = text(item.get("name"), "") or text(item.get("model"), "")
        if name:
            result[normalize_tag(name)] = item
    return result


def ollama_ps(base: str) -> dict[str, dict[str, Any]]:
    try:
        payload = http_json(f"{base}/api/ps", timeout=10.0)
    except (HTTPError, URLError, OSError, json.JSONDecodeError):
        return {}
    result: dict[str, dict[str, Any]] = {}
    for item in as_list(payload.get("models")):
        if not isinstance(item, dict):
            continue
        name = text(item.get("name"), "") or text(item.get("model"), "")
        if name:
            result[normalize_tag(name)] = item
    return result


def tail(path: Path, lines: int = 15) -> str:
    try:
        content = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(content[-lines:])


def memory_available_bytes() -> int | None:
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError):
        return None
    return None


def human_bytes(value: float | int) -> str:
    size = float(value)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if size < 1024 or unit == "TiB":
            return f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} TiB"


# ---------------------------------------------------------------------------
# Starting Ollama
# ---------------------------------------------------------------------------


def ensure_ollama_server(base: str, verbose: bool = True) -> tuple[bool, str]:
    """Make sure an Ollama API answers at `base`; start one if it is loopback."""
    version = ollama_version(base)
    if version is not None:
        if verbose:
            print(f"  ollama: already serving at {base} (v{version})")
        return True, "already serving"

    host = endpoint_host(base)
    if not is_loopback_host(host):
        return (
            False,
            f"{base} is not reachable and is not loopback; start that Ollama "
            "instance yourself",
        )

    ollama = shutil.which("ollama")
    if ollama is None:
        return False, f"no ollama binary on PATH and nothing is serving at {base}"

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    port = endpoint_port(base, DEFAULT_OLLAMA_PORT)
    log_path = LOG_DIR / f"ollama-{port}.log"
    env = os.environ.copy()
    env["OLLAMA_HOST"] = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"

    if verbose:
        print(f"  ollama: no server at {base}; starting `ollama serve` (log {log_path})")
    with log_path.open("ab") as log:
        log.write(f"\n--- {time.strftime('%Y-%m-%d %H:%M:%S')} ollama serve ---\n".encode())
        log.flush()
        process = subprocess.Popen(
            [ollama, "serve"],
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )

    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        if ollama_version(base, timeout=1.0) is not None:
            (LOG_DIR / f"ollama-{port}.pid").write_text(str(process.pid))
            if verbose:
                print(f"  ollama: serving at {base} (pid {process.pid})")
            return True, "started"
        if process.poll() is not None:
            break
        time.sleep(0.5)

    detail = tail(log_path)
    return (
        False,
        f"`ollama serve` did not come up at {base}"
        + (f"\n{detail}" if detail else ""),
    )


def pull_model(base: str, model: str) -> tuple[bool, str]:
    ollama = shutil.which("ollama")
    if ollama is None:
        return False, "no ollama binary on PATH"
    env = os.environ.copy()
    host = endpoint_host(base)
    port = endpoint_port(base, DEFAULT_OLLAMA_PORT)
    env["OLLAMA_HOST"] = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
    print(f"  ollama: pulling {model} (this can take a while)")
    result = subprocess.run([ollama, "pull", model], env=env)
    if result.returncode != 0:
        return False, f"`ollama pull {model}` exited {result.returncode}"
    return True, "pulled"


def warm_model(
    target: Target, base: str, timeout: float
) -> tuple[bool, str]:
    payload: dict[str, Any] = {"model": target.model, "stream": False}
    if target.keep_alive:
        payload["keep_alive"] = target.keep_alive
    if target.options:
        payload["options"] = target.options
    started = time.monotonic()
    try:
        response = http_json(
            ollama_generate_url(target.endpoint), payload=payload, timeout=timeout
        )
    except HTTPError as error:
        return False, http_error_message(error)
    except (URLError, OSError, json.JSONDecodeError) as error:
        return False, str(error)
    if response.get("error"):
        return False, str(response["error"])
    elapsed = time.monotonic() - started
    return True, f"loaded in {elapsed:.1f}s"


# ---------------------------------------------------------------------------
# Starting the bundled llama-server
# ---------------------------------------------------------------------------


def llama_health(base: str) -> str:
    """Return 'ready', 'loading', or 'down' for a llama-server."""
    try:
        with urlopen(f"{base}/health", timeout=2.0) as response:
            return "ready" if response.status == 200 else "loading"
    except HTTPError as error:
        return "loading" if error.code in {503, 500} else "down"
    except (URLError, OSError):
        return "down"


def tcp_reachable(base: str) -> bool:
    parsed = urlparse(base)
    host = parsed.hostname
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    if not host:
        return False
    try:
        with socket.create_connection((host, port), timeout=2.0):
            return True
    except OSError:
        return False


def ensure_llama_server(
    target: Target, wait_seconds: float, no_wait: bool
) -> tuple[bool, str]:
    base = endpoint_base(target.endpoint)
    state = llama_health(base)
    if state == "ready":
        return True, "already serving"
    if state == "loading":
        if no_wait:
            return True, "already loading"
        if wait_for_llama(target, wait_seconds):
            return True, "already serving now"
        return False, "the server is still loading after the wait timeout"

    if target.conflict:
        return False, target.conflict
    model_path = target.model_path
    if model_path is None or not model_path.exists():
        path = model_path or LOCAL_KEV_MODEL
        return (
            False,
            f"the GGUF is not downloaded at {path}; see docs/systemone-local-models.md\n"
            f"  mkdir -p {REPO_ROOT / 'models'} && curl -L -C - -o {path} "
            "https://huggingface.co/ggml-org/Kev-4B-GGUF/resolve/main/Kev-4B-Q4_K_M.gguf",
        )

    script = REPO_ROOT / "scripts" / "run_decision_server.sh"
    if not script.exists():
        return False, f"missing {script}"

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    port = endpoint_port(target.endpoint, 9931)
    log_path = LOG_DIR / f"llama-server-{port}.log"
    env = os.environ.copy()
    env["PORT"] = str(port)
    print(
        f"  llama-server: starting {script.name} with {model_path.name} on port "
        f"{port} (log {log_path})"
    )
    with log_path.open("ab") as log:
        log.write(f"\n--- {time.strftime('%Y-%m-%d %H:%M:%S')} llama-server ---\n".encode())
        log.flush()
        process = subprocess.Popen(
            [str(script), str(model_path)],
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            env=env,
            cwd=str(REPO_ROOT),
            start_new_session=True,
        )
    (LOG_DIR / f"llama-server-{port}.pid").write_text(str(process.pid))

    if no_wait:
        return True, f"started in the background (pid {process.pid})"
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        if llama_health(base) == "ready":
            return True, f"ready (pid {process.pid})"
        if process.poll() is not None:
            detail = tail(log_path)
            return (
                False,
                f"llama-server exited with {process.returncode}"
                + (f"\n{detail}" if detail else ""),
            )
        time.sleep(2.0)
    if process.poll() is None:
        return True, f"started (pid {process.pid}) but still loading after {wait_seconds:.0f}s"
    return False, "llama-server exited before becoming ready"


def wait_for_llama(target: Target, wait_seconds: float) -> bool:
    base = endpoint_base(target.endpoint)
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        if llama_health(base) == "ready":
            return True
        time.sleep(2.0)
    return False


# ---------------------------------------------------------------------------
# Reporting and main
# ---------------------------------------------------------------------------


def describe_target(target: Target) -> str:
    parts = [target.kind, target.model]
    if target.keep_alive:
        parts.append(f"keep_alive={target.keep_alive}")
    if target.options:
        options = ", ".join(f"{k}={v}" for k, v in sorted(target.options.items()))
        parts.append(options)
    return "  ".join(parts)


def print_targets(targets: list[Target]) -> None:
    for target in targets:
        marker = "active" if target.active else "declared"
        print(f"  {describe_target(target)}")
        print(f"      {target.endpoint}  [{marker}]")
        for source in target.sources:
            print(f"      from {source}")
        if target.kind == "llama_server":
            model_path = target.model_path or LOCAL_KEV_MODEL
            state = "found" if model_path.exists() else "MISSING"
            print(f"      GGUF {model_path} ({state})")
        if target.conflict:
            print(f"      conflict: {target.conflict}")


def load_status(targets: list[Target]) -> dict[str, dict[str, dict[str, Any]]]:
    statuses: dict[str, dict[str, dict[str, Any]]] = {}
    for base in {endpoint_base(t.endpoint) for t in targets if t.kind == "ollama"}:
        try:
            statuses[base] = ollama_ps(base)
        except (HTTPError, URLError, OSError, json.JSONDecodeError):
            statuses[base] = {}
    return statuses


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Read AIRogue's settings and start every local model they reference: "
            "local Ollama tags are loaded (and pulled with --pull), and a local_kev "
            "System 1 preset starts the bundled llama-server."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  scripts/start_local_models.py --dry-run\n"
            "  scripts/start_local_models.py\n"
            "  scripts/start_local_models.py --active-only --pull\n"
            "  scripts/start_local_models.py --settings /tmp/kev.json --no-warm\n"
        ),
    )
    parser.add_argument(
        "--settings",
        default="settings.json",
        help="settings file to read (default: settings.json in the current directory)",
    )
    parser.add_argument(
        "--active-only",
        action="store_true",
        help="only start models the current configuration selects, not every "
        "declared ai.profiles / system1.profiles entry",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="print the resolved models and exit"
    )
    parser.add_argument(
        "--pull",
        action="store_true",
        help="pull missing Ollama tags (default: report them and continue)",
    )
    parser.add_argument(
        "--no-warm",
        action="store_true",
        help="ensure servers and tags exist but do not preload the models",
    )
    parser.add_argument(
        "--keep-alive",
        default="30m",
        help="Ollama keep-alive to use when the settings do not set one "
        "(default: 30m; an explicit settings keepAlive wins)",
    )
    parser.add_argument(
        "--load-timeout",
        type=float,
        default=300.0,
        help="seconds to wait for one Ollama load or llama-server start (default: 300)",
    )
    parser.add_argument(
        "--no-wait",
        action="store_true",
        help="for llama-server only: return after spawning it instead of waiting "
        "for /health",
    )
    args = parser.parse_args(argv)

    settings_path = Path(args.settings).expanduser()
    root, used_files = load_settings(settings_path)
    profiles = load_profiles(root)
    collector = collect_targets(
        root, profiles, default_keep_alive=args.keep_alive, active_only=args.active_only
    )
    targets = list(collector.targets.values())
    targets.sort(key=lambda target: not target.active)

    print("AIRogue local models")
    for path in used_files:
        print(f"  settings: {path}")
    if args.active_only:
        print("  scope: active configuration only")
    else:
        print("  scope: every local model named in the settings")

    if not targets:
        print("\nNo local models found in these settings.")
        for note in collector.notes:
            print(f"  note: {note}")
        return 0

    print("\nModels:")
    print_targets(targets)
    for note in collector.notes:
        print(f"  note: {note}")

    if args.dry_run:
        print("\nDry run: no servers were contacted and no models were loaded.")
        return 0

    failures: list[str] = []
    warnings: list[str] = []

    def problem(message: str, active: bool) -> None:
        (failures if active else warnings).append(message)

    print("\nStarting backends")

    # One server per endpoint, then one load per model. Sequential on purpose:
    # loading several multi-GB models at once is how a 15 GiB box swaps.
    ollama_ready: dict[str, bool] = {}
    for target in targets:
        if target.kind != "ollama":
            continue
        base = endpoint_base(target.endpoint)
        if base not in ollama_ready:
            ok, detail = ensure_ollama_server(base)
            ollama_ready[base] = ok
            if not ok:
                base_has_active = any(
                    t.kind == "ollama"
                    and endpoint_base(t.endpoint) == base
                    and t.active
                    for t in targets
                )
                problem(f"ollama at {base}: {detail}", base_has_active)

    installed: dict[str, dict[str, dict[str, Any]]] = {}
    for base, ok in ollama_ready.items():
        if ok:
            try:
                installed[base] = ollama_tags(base)
            except (HTTPError, URLError, OSError, json.JSONDecodeError) as error:
                base_has_active = any(
                    t.kind == "ollama"
                    and endpoint_base(t.endpoint) == base
                    and t.active
                    for t in targets
                )
                problem(f"ollama at {base}: could not list models: {error}", base_has_active)
                installed[base] = {}
    if not args.active_only and not args.no_warm:
        memory_warning(targets, installed)

    for target in targets:
        if target.kind != "ollama":
            continue
        base = endpoint_base(target.endpoint)
        if not ollama_ready.get(base):
            problem(f"{target.model}: Ollama is not serving at {base}", target.active)
            continue

        tag = normalize_tag(target.model)
        if tag not in installed.get(base, {}):
            if not args.pull:
                problem(
                    f"{target.model}: not installed on {base} "
                    "(re-run with --pull or run `ollama pull " + target.model + "`)",
                    target.active,
                )
                continue
            ok, detail = pull_model(base, target.model)
            if not ok:
                problem(f"{target.model}: {detail}", target.active)
                continue
            try:
                installed[base] = ollama_tags(base)
            except (HTTPError, URLError, OSError, json.JSONDecodeError):
                installed[base] = {}

        if args.no_warm:
            print(f"  ollama: {target.model} ready (--no-warm, not loaded)")
            continue
        print(f"  ollama: loading {target.model} on {base}")
        ok, detail = warm_model(target, base, timeout=args.load_timeout)
        if ok:
            print(f"  ollama: {target.model} {detail}")
        else:
            problem(f"{target.model} at {base}: {detail}", target.active)

    for target in targets:
        if target.kind == "llama_server":
            ok, detail = ensure_llama_server(
                target, wait_seconds=args.load_timeout, no_wait=args.no_wait
            )
            if ok:
                print(f"  llama-server: {detail}")
            else:
                problem(f"llama-server at {target.endpoint}: {detail}", target.active)
        elif target.kind == "external":
            base = endpoint_base(target.endpoint)
            if tcp_reachable(base):
                print(f"  external: {target.model} is reachable at {target.endpoint}")
            else:
                problem(
                    f"{target.model}: {target.endpoint} is local but not reachable, "
                    "and this script has no preset to start it",
                    target.active,
                )

    if any(t.kind == "ollama" for t in targets):
        statuses = load_status(targets)
        print("\nOllama resident now:")
        for target in targets:
            if target.kind != "ollama":
                continue
            base = endpoint_base(target.endpoint)
            entry = statuses.get(base, {}).get(normalize_tag(target.model))
            if entry:
                expires = text(entry.get("expires_at"), "?")
                size = integer(entry.get("size"), 0)
                print(f"  {target.model:<28} {human_bytes(size):>9}  expires {expires}")
            else:
                print(f"  {target.model:<28} not resident")
                if not args.no_warm:
                    problem(
                        f"{target.model}: not resident after loading "
                        "(Ollama may have evicted it for memory)",
                        target.active,
                    )

    if warnings:
        print("\nProblems with declared-but-unselected models:")
        for warning in warnings:
            print(f"  - {warning}")

    if failures:
        print("\nProblems:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("\nAll selected local models are running.")
    if warnings:
        print("Declared-but-unselected models that could not be started are listed above.")
    return 0


def memory_warning(
    targets: list[Target], installed: dict[str, dict[str, dict[str, Any]]]
) -> None:
    available = memory_available_bytes()
    if available is None:
        return
    total = 0
    for target in targets:
        if target.kind != "ollama":
            continue
        base = endpoint_base(target.endpoint)
        entry = installed.get(base, {}).get(normalize_tag(target.model))
        if entry:
            total += integer(entry.get("size"), 0)
    if total > available * 0.9:
        print(
            f"  warning: the requested Ollama models total about {human_bytes(total)} "
            f"but only {human_bytes(available)} is available; Ollama may evict earlier "
            "models. `--active-only` starts fewer."
        )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        raise SystemExit(130)
