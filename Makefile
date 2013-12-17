## Copyright (c) 2013, Linaro Limited
## All rights reserved.
##
## Redistribution and use in source and binary forms, with or without
## modification, are permitted provided that the following conditions are met:
##
##    * Redistributions of source code must retain the above copyright notice, this
##      list of conditions and the following disclaimer.
##
##    * Redistributions in binary form must reproduce the above copyright notice, this
##      list of conditions and the following disclaimer in the documentation and/or
##      other materials provided with the distribution.
##
##    * Neither the name of Linaro Limited nor the names of its contributors may be
##      used to endorse or promote products derived from this software without specific
##      prior written permission.
##
## THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
## ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
## WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
## DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
## FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
## DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
## SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
## CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
## OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
## OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


ODP_ROOT = .
ARCH     = linux-generic
ODP_LIB  = $(ODP_ROOT)/arch/$(ARCH)
OBJ_DIR  = ./obj
LIB      = $(ODP_LIB)/lib/odp.a
ODP_APP  = odp_app
ODP_TESTS = $(ODP_ROOT)/test
INCLUDE  = -I$(ODP_ROOT)/include
CC       = @gcc

.PHONY: all
all: libs tests

.PHONY: tests
tests:
	$(MAKE) -C $(ODP_TESTS)

.PHONY: docs
docs:
	$(MAKE) -C $(ODP_LIB) docs

.PHONY: libs
libs:
	$(MAKE) -C $(ODP_LIB) all

.PHONY: clean
clean:
	$(MAKE) -C $(ODP_LIB) clean
	$(MAKE) -C $(ODP_TESTS) clean

.PHONY: install
install:
	$(MAKE) -C $(ODP_LIB) install
	$(MAKE) -C test install
