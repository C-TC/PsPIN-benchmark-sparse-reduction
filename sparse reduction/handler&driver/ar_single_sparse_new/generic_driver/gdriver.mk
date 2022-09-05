all::
	make deploy
	make driver
	make driver_debug

$(GENERIC_DRIVER_DIR)/gdriver_args.c: $(GENERIC_DRIVER_DIR)/gdriver_args.ggo
	gengetopt -i $(GENERIC_DRIVER_DIR)/gdriver_args.ggo -F gdriver_args --output-dir=$(GENERIC_DRIVER_DIR)/

driver: driver/driver.c $(GENERIC_DRIVER_DIR)/gdriver_args.c $(GENERIC_DRIVER_DIR)/gdriver.c
	 gcc -std=c99 $(ALLREDUCE_FLAGS) -I$(GENERIC_DRIVER_DIR)/ -I$(PSPIN_RT)/runtime/include/ -I$(PSPIN_HW)/verilator_model/include driver/driver.c $(GENERIC_DRIVER_DIR)/gdriver.c $(GENERIC_DRIVER_DIR)/gdriver_args.c ./set/src/set.c -L$(PSPIN_HW)/verilator_model/lib/ -lpspin -lm -o sim_${SPIN_APP_NAME}

driver_debug: driver/driver.c $(GENERIC_DRIVER_DIR)/gdriver_args.c
	 gcc -g -std=c99 $(ALLREDUCE_FLAGS) -I$(GENERIC_DRIVER_DIR)/ -I$(PSPIN_RT)/runtime/include/ -I$(PSPIN_HW)/verilator_model/include driver/driver.c $(GENERIC_DRIVER_DIR)/gdriver.c $(GENERIC_DRIVER_DIR)/gdriver_args.c ./set/src/set.c -L$(PSPIN_HW)/verilator_model/lib/ -lpspin_debug -lm -o sim_${SPIN_APP_NAME}_debug

clean::
	-@rm *.log 2>/dev/null || true
	-@rm -r build/ 2>/dev/null || true
	-@rm -r waves.vcd 2>/dev/null || true
	-@rm sim_${SPIN_APP_NAME} 2>/dev/null || true
	-@rm sim_${SPIN_APP_NAME}_debug 2>/dev/null || true

run::
	./sim_${SPIN_APP_NAME}

.PHONY: driver driver_debug clean run
