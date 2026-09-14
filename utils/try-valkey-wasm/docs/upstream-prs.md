# Upstream PR drafts

Both branches are on valkey-rainfall/valkey, one commit each, off `unstable` @ `f42c9ab3d`, DCO-signed.
Open them with the compare links (gh pr create cannot target valkey-io from this token); paste title and
body below.

---

## PR 1 -- Fix return types of three module API declarations

Branch: `fix/module-api-return-types` (809f645cf)
Open: https://github.com/valkey-io/valkey/compare/unstable...valkey-rainfall:valkey:fix/module-api-return-types?expand=1

**Title:** Fix return types of three module API declarations

**Body:**

`ValkeyModule_FreeModuleUser`, `ValkeyModule_ACLAddLogEntry` and `ValkeyModule_ACLAddLogEntryByUserName` are declared in `valkeymodule.h` as returning `void`, but the implementations (`VM_FreeModuleUser`, `VM_ACLAddLogEntry`, `VM_ACLAddLogEntryByUserName`) return `int`, and the latter two document `VALKEYMODULE_OK` / `VALKEYMODULE_ERR`.

A module calling them through the API table therefore calls an `int` function through a `void` function pointer, which is undefined behavior in C. On x86-64 and arm64 the discarded return register makes it harmless in practice. On a target with strict indirect-call signature checking (WebAssembly) the call traps: `redis.acl_check_cmd` in a Lua script, which calls `FreeModuleUser` through the API table, fails with `null function or function signature mismatch`.

This PR declares them as `int` to match the implementation. Existing modules that ignore the return value are source- and ABI-compatible; modules can now check the result of the ACL log calls as their documentation already says they can. `VM_FreeModuleUser` gets a one-line doc note that it returns `VALKEYMODULE_OK`. `redismodule.h` only aliases these names, so nothing else changes.

I checked every `ValkeyModule_*` declaration against its `VM_*` implementation with a small script; these three are the only return-type mismatches (the remaining differences are typedef aliases such as `robj` vs `ValkeyModuleString`). Happy to contribute that check as a CI step in a follow-up if there is interest.

Verified: full build; `tests/modules` build; `unit/moduleapi/aclcheck`, `auth`, `usercall`, `moduleauth` pass (62/62).

---

## PR 2 -- Fix getTimeZone() on non-Linux platforms

Branch: `fix/gettimezone-portable` (f306b739d)
Open: https://github.com/valkey-io/valkey/compare/unstable...valkey-rainfall:valkey:fix/gettimezone-portable?expand=1

**Title:** Fix getTimeZone() on non-Linux platforms

**Body:**

On platforms other than Linux and Solaris, `getTimeZone()` read the `timezone` argument of `gettimeofday()`. That argument is obsolete: POSIX specifies it as unused, glibc and musl fill it with zero (or leave it untouched), and BSD kernels return whatever was last set with `settimeofday()`, normally zero. The result is an offset of zero -- or garbage -- and log timestamps in the wrong zone on every non-Linux build. (Found compiling the server with Emscripten, whose libc is musl-derived: the log timestamps read `-1057815 Jan 1970`.)

This PR derives the offset from the C library's own conversion instead: the difference between `localtime_r()` and `gmtime_r()` of the same instant, with day and year straddles handled, and the DST hour removed so the value has the same meaning as the `timezone` global on Linux -- standard-time seconds west of UTC -- which is what `nolocks_localtime()` and `formatTimezone()` expect (they add `3600 * daylight_active` themselves). No platform extensions (`tm_gmtoff`, `timegm`) are used, so this also holds on AIX and Haiku, which the Makefile lists.

Verification: since the branch is not compiled on Linux, I compared the new computation against glibc's `timezone` on Linux for 14 zones (including half-hour and 45-minute offsets, southern-hemisphere DST, and the year boundary) at four instants. Identical in every case except Lord Howe Island, whose DST shift is 30 minutes rather than 60: there the new code yields the correct displayed local time under the callers' fixed one-hour DST assumption, where `timezone` would be 30 minutes off. I also compiled `util.c` with `__linux__` undefined to exercise the branch. I do not have a macOS or BSD machine to run the server on; a reviewer on one of those could confirm that `INFO server` / log timestamps now show the local zone.
