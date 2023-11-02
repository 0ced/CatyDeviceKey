#include "common.h"
#include "main.h"

uint32_t g_alreadyStarted = 0;

int __entry(int argc, char **argv) {

	InitOSFunctionPointers();
	InitVPadFunctionPointers();
	InitGX2FunctionPointers();
	InitSysFunctionPointers();
	InitSocketFunctionPointers();

	if(g_alreadyStarted)
		return 0;

	g_alreadyStarted++;

	init_kernel_syscalls();

	log_init();
	log_printf("\nMCP MADE BY CEDKECHAT001\n");
	log_printf("MEN BRANLE DU BLE FRR\n\n");

	return Launcher(argc, argv);

}