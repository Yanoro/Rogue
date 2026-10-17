#!/usr/bin/env bash
# Ask a running local decision server the same three questions AIRogue's
# --systemone-test asks, so a local model and hosted Jev can be compared on one
# identical prompt.
#
# Why this exists next to --systemone-test: that mode goes through
# SystemOne::JevClassifier, which is the thing being trusted, so it cannot also be
# the thing that proves the server is up. This is the raw curl, with no engine in
# the middle -- if this works and the engine does not, the bug is in the provider.
#
# Usage:
#   scripts/test_decision_server.sh                 # default port 9931
#   PORT=8081 scripts/test_decision_server.sh
set -euo pipefail

PORT="${PORT:-9931}"
URL="http://127.0.0.1:${PORT}/v1/systemone"
RESPONSE_FILE="$(mktemp)"
trap 'rm -f "$RESPONSE_FILE"' EXIT

# Deliberately the same state and questions as RunSystemOneTest in src/main.cpp:
# a basket holding one red apple, and no number seven anywhere. Both correct
# answers are therefore known, which is what makes a wrong reply distinguishable
# from a wrong parse.
read -r -d '' BODY <<'JSON' || true
{
  "model": "local",
  "state": "A basket on the table holds a single red apple. There are no numbers written anywhere in this scene.",
  "questions": {
    "has_seven": {
      "type": "noul",
      "instructions": "The state mentions the number seven."
    },
    "fruit": {
      "type": "choice",
      "instructions": "Which fruit does the state describe?",
      "criteria": {
        "apple": "A red apple.",
        "banana": "A yellow banana.",
        "none": "No fruit is described."
      }
    },
    "table_emptiness": {
      "type": "score",
      "instructions": "How empty is the table?",
      "criteria": ["Completely empty.", "Holds one object.", "Crowded with objects."]
    }
  }
}
JSON

echo "POST $URL"
echo

# --max-time rather than a bare curl: a local server still loading its model
# accepts the connection and then stalls, and a hang would read as a broken
# script rather than a starting server. The body is written to a file so the
# verdict below parses it exactly once -- parsing the same stdin twice finds
# nothing the second time and reports a bogus "reply was not JSON".
if ! curl -sS --max-time 180 -H "Content-Type: application/json" \
        -d "$BODY" "$URL" -o "$RESPONSE_FILE" 2>/dev/null; then
  echo "request failed; is the server running?" >&2
  echo "  scripts/run_decision_server.sh" >&2
  exit 1
fi

if [[ ! -s "$RESPONSE_FILE" ]]; then
  echo "empty reply -- the server is probably still loading the model" >&2
  exit 1
fi

python3 -m json.tool < "$RESPONSE_FILE" 2>/dev/null || cat "$RESPONSE_FILE"

echo
# Both envelopes are accepted. In practice llama.cpp master returns the same
# {"answers": {...}} shape as hosted OpenRouter, but the chat-wrapped variant is
# what some other local servers emit, and telling them apart is the whole reason
# this check states the envelope rather than leaving it to be eyeballed.
python3 - "$RESPONSE_FILE" <<'PY'
import json, sys
with open(sys.argv[1]) as handle:
    root = json.load(handle)

if "answers" in root:
    envelope = "'answers' object (same shape as hosted OpenRouter)"
    answers = root["answers"]
elif "choices" in root and root["choices"]:
    envelope = "'choices'/'message'/'content' (chat-wrapped)"
    content = root["choices"][0].get("message", {}).get("content")
    answers = json.loads(content) if isinstance(content, str) else content
else:
    print("FAILED: neither an 'answers' object nor a choices envelope")
    print("keys present:", sorted(root.keys()))
    raise SystemExit(1)

print("envelope:", envelope)

ok = True
fruit = answers.get("fruit", {}).get("choice")
if fruit != "apple":
    print(f"FAILED: fruit is {fruit!r}, expected 'apple'")
    ok = False
else:
    print("fruit:            apple (correct)")

seven = answers.get("has_seven", {}).get("noul")
if seven is None:
    print("FAILED: has_seven has no 'noul' value")
    ok = False
elif seven > 0.5:
    print(f"FAILED: has_seven is {seven}, expected a low probability")
    ok = False
else:
    print(f"has_seven:        {seven:.4f} (correct, low)")

print("usage:", root.get("usage", "(none)"))
print("server: ok" if ok else "server: FAILED")
raise SystemExit(0 if ok else 1)
PY
