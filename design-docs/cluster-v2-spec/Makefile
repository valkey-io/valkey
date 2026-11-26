# Run TLA+ tools in a podman container. This folder is mounted at /work and
# the tools run in /work/spec, so TLC's output (states/, traces) lands in spec/.
#
#   make image                  build the tools image (once; needs network)
#   make parse                  syntax and semantic check (SANY)
#   make tlc                    exhaustive model check with the .cfg
#   make simulate DEPTH=100     random behaviours, for state spaces too big for tlc
#   make canary                 each Canary* invariant alone; each must be violated
#   make pcal SPEC=<module>     translate PlusCal in place
#   make shell                  a shell in the container
#   make clean                  remove TLC state and trace files
#
# Override any variable on the command line, e.g.
#   make tlc SPEC=ValkeyRaft CFG=MCValkeyRaft.cfg HEAP=8g WORKERS=4

SPEC_DIR := spec
TOOLS_DIR := tools

SPEC     ?= ValkeyRaft
CFG      ?= MC$(SPEC).cfg
IMAGE    ?= tla-tools
PODMAN   ?= podman
HEAP     ?= 4g
WORKERS  ?= auto
DEPTH    ?= 100
# -deadlock turns off TLC's deadlock check. Needed while actions are stubs
# (nothing is enabled), and usually wanted for protocol specs anyway.
TLC_OPTS ?= -deadlock

# --network=none   the run needs no network
# --userns=keep-id files TLC writes (states/, traces) are owned by you
# No :Z on the mount: Linux Mint uses AppArmor, not SELinux. Add it on Fedora.
RUN  = $(PODMAN) run --rm --network=none --userns=keep-id \
         -v "$(CURDIR)":/work -w /work/$(SPEC_DIR) $(IMAGE)
JAVA = $(RUN) java -Xmx$(HEAP) -XX:+UseParallelGC -cp /opt/tla/tla2tools.jar

.PHONY: image parse tlc simulate canary pcal shell clean

# Build context is tools/, which holds only the Containerfile, so nothing
# else (spec files, TLC state) is sent to the build.
image:
	$(PODMAN) build -t $(IMAGE) -f $(TOOLS_DIR)/Containerfile $(TOOLS_DIR)

parse:
	$(JAVA) tla2sany.SANY $(SPEC).tla

tlc: parse
	$(JAVA) tlc2.TLC -config $(CFG) -workers $(WORKERS) -metadir states \
	  $(TLC_OPTS) $(SPEC).tla

simulate: parse
	$(JAVA) tlc2.TLC -config $(CFG) -simulate -depth $(DEPTH) -metadir states \
	  $(TLC_OPTS) $(SPEC).tla

# Vacuity check. A canary says something ordinary never happens; if TLC
# does not report it violated, the bounds or a guard made that behaviour
# unreachable and a clean `make tlc` proves nothing about it. Runs the cfg
# with its INVARIANTS section (which must be last) replaced by one canary.
# sed, the redirect and grep run on the host, so their paths include spec/.
CANARIES ?= CanaryLeaderChange CanaryCommit CanaryFollowerApply CanaryVoterChange
canary: parse
	@fail=0; for c in $(CANARIES); do \
	  sed '/^INVARIANTS/,$$d' $(SPEC_DIR)/$(CFG) > $(SPEC_DIR)/canary.cfg; \
	  echo "INVARIANT $$c" >> $(SPEC_DIR)/canary.cfg; \
	  $(JAVA) tlc2.TLC -config canary.cfg -workers $(WORKERS) \
	    -metadir states/canary $(TLC_OPTS) $(SPEC).tla > $(SPEC_DIR)/canary.out 2>&1; \
	  if grep -q "Invariant $$c is violated" $(SPEC_DIR)/canary.out; then \
	    echo "ok    $$c (violated, as it must be)"; \
	  else echo "FAIL  $$c not violated: see $(SPEC_DIR)/canary.out"; fail=1; break; fi; \
	done; rm -f $(SPEC_DIR)/canary.cfg; exit $$fail

pcal:
	$(JAVA) pcal.trans $(SPEC).tla

shell:
	$(PODMAN) run --rm -it --network=none --userns=keep-id \
	  -v "$(CURDIR)":/work -w /work/$(SPEC_DIR) $(IMAGE) bash

clean:
	cd $(SPEC_DIR) && rm -rf states *_TTrace_*.tla *_TTrace_*.bin *.old canary.cfg canary.out
