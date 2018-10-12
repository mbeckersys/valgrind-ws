SCRIPTS=$(wildcard *.py)
WORKLOADS=$(SCRIPTS:.py=.log)

.PHONY: announce summarize

all: | announce $(WORKLOADS) summarize

announce:
	@echo "Workloads: $(WORKLOADS)"

summarize:
	@echo "Tests finished"

%.log: %.py
	@echo "Running test $< ..."
	./$< &> $@

install:

clean:
	rm -f *.log
