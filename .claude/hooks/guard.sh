#!/bin/sh
# Blocks the few actions the user never wants (see CLAUDE.md). Exit 2 = block, stderr goes to Claude.
in=$(cat)
cmd=$(printf '%s' "$in" | jq -r '.tool_input.command // empty')
text=$(printf '%s' "$in" | jq -r '(.tool_input.new_string // "") + (.tool_input.content // "")')
case "$cmd" in
  *screencapture*) printf '%s' "$cmd" | grep -q -- '-l' || { echo "Full-screen screencapture grabs the user's other work: capture a window id (-l)." >&2; exit 2; } ;;
esac
if printf '%s' "$cmd" | grep -Eq 'git push +origin|Boxingbruin'; then
  echo "Never push or publish to upstream (origin/Boxingbruin): use the cbyeh fork." >&2; exit 2
fi
path=$(printf '%s' "$in" | jq -r '.tool_input.file_path // empty')
if [ "${path%.md}" = "$path" ] && printf '%s' "$text" | grep -q 'display_set_fps_limit'; then
  echo "Never lock or cap the frame rate (CLAUDE.md)." >&2; exit 2
fi
exit 0
