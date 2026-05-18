#include <nds.h>
#include <dswifi9.h>
#include <wfc.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <netdb.h>

#include "romm.h"

#define VERSION				ROMM_VERSION

#define CRC32_REMAINDER		0xFFFFFFFF

#define SEND_CRC_CHECK		1
#define SEND_VERIFIED		2

#define DEFAULT_PORT		1500

#define MAXPATHLEN			64


// File info struct
typedef struct {
	u_int	flags;
	u_int	fileSize;
	u_int	fileCRC32;
	int		nameLen;
} FILEHEAD;


char	buff[32768];			// Data buffer
char	currentDir[MAXPATHLEN];	// Current directory
int		systemShutdown = false;	// Flag for auto-shutdown


void DoComms(int sock);
void SendReturn(int sock, int code);

bool CreatePath(const char *path);
int GetFreeDiskSpace(const char *path);

unsigned int crc32(void* buff, int bytes, unsigned int crc);

void run_server_mode(void);

/* ---------------------------------------------------------------------------
 * Connect to WiFi trying every saved profile (DS slots 1-3, DSi slots 4-6).
 * Returns true if connected, false on failure/timeout.
 * --------------------------------------------------------------------------- */
static bool wifi_connect_all_slots(void) {
	/* Init hardware + WFC subsystem only (no auto-connect yet) */
	if (!Wifi_InitDefault(INIT_ONLY)) {
		printf("Hardware init failed.\n");
		return false;
	}

	/* Load ALL saved profiles from NVRAM:
	 *  DS firmware  → slots 1-3
	 *  DSi NVRAM    → slots 4-6 (wfcLoadFromNvram checks g_isTwlMode) */
	wfcLoadFromNvram();
	unsigned nslots = wfcGetNumSlots();
	printf("%u profile%s found.\n", nslots, nslots == 1 ? "" : "s");

	if (nslots == 0) {
		printf("No WiFi profiles configured.\n");
		return false;
	}

	/* Start trying all valid profiles in order */
	if (!wfcBeginAutoConnect()) {
		printf("Auto-connect start failed.\n");
		return false;
	}

	/* Poll until connected or all profiles exhausted.
	 * Give a 2-second grace period before treating Disconnected as failure
	 * (status can briefly read 0 right after wfcBeginAutoConnect). */
	int grace   = 120; /* 2 s @ 60 fps */
	int timeout = 60 * 30; /* 30 s max */
	WfcStatus prev = (WfcStatus)-1;

	while (timeout-- > 0) {
		swiWaitForVBlank();
		WfcStatus s = (WfcStatus)wfcGetStatus();

		if (s != prev) {
			switch (s) {
				case WfcStatus_Scanning:
					printf("\rScanning...      "); break;
				case WfcStatus_Connecting:
					printf("\rConnecting...    "); break;
				case WfcStatus_AcquiringIP:
					printf("\rAcquiring IP...  "); break;
				case WfcStatus_Connected:
					printf("\rConnected!       \n"); return true;
				default: break;
			}
			prev = s;
		}

		if (s == WfcStatus_Connected)   return true;
		if (s == WfcStatus_Disconnected && grace <= 0) break;
		if (grace > 0) grace--;
	}

	return (WfcStatus)wfcGetStatus() == WfcStatus_Connected;
}


int main(void) {

	// Simple init (turn off bottom screen and set up text console)
	powerOff(PM_BACKLIGHT_BOTTOM);

	videoSetMode(MODE_0_2D);
	vramSetBankA(VRAM_A_MAIN_BG);

	consoleInit(NULL, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);


	// Display program banner
	printf("DS File Loader Utility v%s\n", VERSION);
	printf("2016-2025 Meido-Tek Productions\n");
	printf("v2 RoMM fork by VXisto\n\n");

	printf("Initializing File System...");

	if (!fatInitDefault()) {

		printf("\nERROR: Cannot init FS.");
		while(1) swiWaitForVBlank();

	}

	printf("Ok!\n");

	// Get current directory
	getcwd(currentDir, MAXPATHLEN);


	printf("Initializing WiFi...\n");

	if (!wifi_connect_all_slots()) {

		printf("\nCould not connect to any WiFi profile.\n");
		printf("Check DSi/DS WiFi settings (slots 1-6).\n");
		while(1) swiWaitForVBlank();

	} else {

		printf("Ok!\n\n");

		// Display wifi status
		{
			struct in_addr ip, gateway, mask, dns1, dns2;
			ip = Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);
			printf("Device IP   : %s\n", inet_ntoa(ip));
		}

		// Load RoMM config (best-effort; doesn't fail if absent)
		RommConfig romm_cfg;
		int has_romm = romm_load_config(&romm_cfg);

		// Main menu
		printf("\n");
		printf("A:     Server Mode\n");
		printf("B:     RoMM Download\n");
		printf("SEL:   Check for Updates\n");
		printf("START: Shutdown\n");
		if (!has_romm)
			printf("[No /romm.cfg found]\n");

		while(pmMainLoop()) {

			scanKeys();
			int keys = keysDown();

			if (keys & KEY_A) {
				run_server_mode();
				break;
			} else if ((keys & KEY_B) && has_romm) {
				romm_run_mode(&romm_cfg);
				break;
			} else if ((keys & KEY_B) && !has_romm) {
				printf("Create /romm.cfg first!\n");
			} else if (keys & KEY_SELECT) {
				/* Auto-updater is not yet operational.
				 * The infrastructure (version manifest + HTTP delivery
				 * from a local server) is designed but needs proper
				 * testing before shipping.  TBD. */
				consoleClear();
				printf("=== Check for Updates ===\n\n");
				printf("Auto-updater is not yet\n");
				printf("available in this build.\n\n");
				printf("Check for releases at:\n");
				printf("github.com/VXisto/\n");
				printf("dsloadbump\n\n");
				printf("(press any key)\n");
				/* Wait for any button press */
				while (1) {
					swiWaitForVBlank();
					scanKeys();
					if (keysDown()) break;
				}
				consoleClear();
		printf("DS File Loader Utility v%s\n", VERSION);
		printf("2016-2025 Meido-Tek Productions\n");
		printf("v2 RoMM fork by VXisto\n\n");
				printf("Device IP   : %s\n", inet_ntoa(Wifi_GetIPInfo(NULL,NULL,NULL,NULL)));
				printf("\n");
				printf("A:     Server Mode\n");
				printf("B:     RoMM Download\n");
				printf("SEL:   Check for Updates\n");
				printf("START: Shutdown\n");
				if (!has_romm) printf("[No /romm.cfg found]\n");
			} else if (keys & KEY_START) {
				systemShutdown = true;
				break;
			}

			swiWaitForVBlank();

		}

	}

	return 0;

}


void run_server_mode(void) {

	consoleClear();
	printf("=== Server Mode ===\n\n");
	printf("Default Dir : %s\n\n", currentDir);

	// Create a socket as a server and bind it
	struct sockaddr_in server,client;
	int sock,csock;
	int ret;

	sock = socket(AF_INET, SOCK_STREAM, 0);

	server.sin_family		= AF_INET;
	server.sin_port			= htons(DEFAULT_PORT);
	server.sin_addr.s_addr	= INADDR_ANY;

	ret = bind(sock, (struct sockaddr*) &server, sizeof(server));

	if (ret) {

		printf("Error %d binding socket!\n", ret);
		while(1) swiWaitForVBlank();

	} else {

		printf("Waiting for connection...\n");
		printf("Free space: %.2fMB\n", (((float)GetFreeDiskSpace("/"))/1024)/1024);

		// Begin listening for incoming connections
		if ((ret = listen(sock, 5))) {

			printf("Error %d listening!\n", ret);
			while(1) swiWaitForVBlank();

		} else {

			// Server accept loop
			while(1) {

				// Check and accept an incoming connection
				{
					socklen_t clientlen = sizeof(client);
					csock = accept(sock, (struct sockaddr *)&client, &clientlen);
				}

				// Is socket valid?
				if (csock >= 0) {

					int isClient = false;

					// Test comms with client
					{

						char temp[6];
						memset(temp, 0, 6);
						printf("Connected with %s\n", inet_ntoa(client.sin_addr));

						// Receive an ID from the client
						if (recv(csock, temp, 5, 0) > 0) {

							// Check if message is a SNC0 ID
							if (strncmp("SYNC0", temp, 5) != 0) {

								printf("ERRROR: Client not compatible.\n");

							} else {

								printf("Protocol accepted!\n");
								isClient = true;
								SendReturn(csock, 0);

							}

						} else {

							printf("ERROR: Client ID receive fail.\n");

						}

					}

					// If client is valid, begin comms
					if (isClient) {

						DoComms(csock);
						printf("Client disconnected.\n");
						chdir(currentDir);

					}

					// Close connection
					shutdown(csock, 0);
					closesocket(csock);

					// Exit loop if shutdown flag is set
					if (systemShutdown == true)
						break;

					// Display stats
					printf("Waiting for connection...\n");
					printf("Free space: %.2fMB\n", (((float)GetFreeDiskSpace("/"))/1024)/1024);

				}

				swiWaitForVBlank();

			}

		}

	}

}


void DoComms(int sock) {

	char cmd[5];

	while(1) {

		memset(cmd, 0x00, 5);

        if (recv(sock, cmd, 4, 0) > 0) {

			// Close connection
			if (strcmp("CLCN", cmd) == 0) {

				break;

			// System shutdown
			} else if (strcmp("STDN", cmd) == 0) {

				systemShutdown = true;
				break;

			// Change directory
			} else if (strcmp("CHDR", cmd) == 0) {

				SendReturn(sock, 0);

				int	pathLen;
				recv(sock, &pathLen, 4, 0);

                char pathName[pathLen];
				recv(sock, &pathName, pathLen, 0);

				if (chdir(pathName)) {

					printf("Unable to set current dir to:\n%s\n", pathName);
					SendReturn(sock, 1);

				} else {

					printf("Set current dir to:\n%s\n", pathName);
					SendReturn(sock, 0);

                }

			// Send file request
			} else if (strcmp("SNFL", cmd) == 0) {

                printf("File Send command received.\n");

				// Send return code
                SendReturn(sock, 0);

				// Receive file parameters
                FILEHEAD file;
                recv(sock, &file, sizeof(file), 0);

				// Receive file name
				char fileName[file.nameLen+1];
				memset(fileName, 0x00, file.nameLen+1);
				recv(sock, fileName, file.nameLen, 0);

				printf("Downloading %s...", fileName);

				SendReturn(sock, 0);

				FILE *fp = fopen(fileName, "wb");

				if (fp == NULL) {

					printf("ERROR: Cannot create file.\n");

				} else {

					int bytesLeft = file.fileSize;
					int count = 0;
					unsigned int checksum = CRC32_REMAINDER;
					unsigned int compsum;

                    while(bytesLeft > 0) {

						int bytesToReceive = bytesLeft;

						if (bytesToReceive > 32768)
							bytesToReceive = 32768;

						int ret;

						if (file.flags & SEND_VERIFIED)
							ret = recv(sock, &checksum, 4, 0);
						else
							ret = 1;

						if (ret < 0) {

							printf("ERROR: Download fail.\n");
							break;

						}

						int received;

						while(1) {

							received = 0;

							while(received < bytesToReceive) {

								ret = recv(sock, &buff[received], bytesToReceive-received, 0);

								if (ret < 0)
									break;

								received += ret;

							}

							if (ret < 0)
								break;

							if (file.flags & SEND_VERIFIED) {

								compsum = crc32(buff, received, CRC32_REMAINDER);

								if (checksum == compsum) {

									SendReturn(sock, 0);
									break;

								} else {

									SendReturn(sock, 1);
									printf("X");

								}

							} else {

								break;

							}

						}

                        if (ret < 0) {

							printf("\nERROR: Download fail.\n");
							break;

                        } else if (ret >= 0) {

							fwrite(buff, 1, received, fp);
							fflush(fp);

							bytesLeft -= received;

                        }


						if (count >= 2) {
							printf(".");
							count = 0;
						}

                        count++;


                    }

					fflush(fp);
					fclose(fp);

					fp = fopen("_$dummy", "wb");
					fwrite(buff, 1, 32768, fp);
					fflush(fp);
					fclose(fp);

                    unlink("_$dummy");

					printf("\n");

					if (bytesLeft == 0) {
						SendReturn(sock, 0);
						printf("Downloaded successful.\n");
					}

				}

			} else if (strlen(cmd) > 0) {

				printf("Unknown command: %s\n", cmd);

			}

        } else {

        	break;

        }

		swiWaitForVBlank();

	}

}

void SendReturn(int sock, int code) {

	send(sock, (char*)&code, 4, 0);

}

int GetFreeDiskSpace(const char *path) {

	struct statvfs diskStat;
    statvfs(path, &diskStat);

	return(diskStat.f_bsize*diskStat.f_bfree);

}

void _crc32_init(unsigned int* table) {

	int i,j;
	unsigned int crcVal;

	for(i=0; i<256; i++) {

		crcVal = i;

		for(j=0; j<8; j++) {

			if (crcVal&0x00000001L)
				crcVal = (crcVal>>1)^0xEDB88320L;
			else
				crcVal = crcVal>>1;

		}

		table[i] = crcVal;

	}

}

unsigned int crc32(void* buff, int bytes, unsigned int crc) {

	int	i;
	unsigned char*	byteBuff = (unsigned char*)buff;
	unsigned int	byte;
	unsigned int	crcTable[256];

    _crc32_init(crcTable);

	for(i=0; i<bytes; i++) {

		byte = 0x000000ffL&(unsigned int)byteBuff[i];
		crc = (crc>>8)^crcTable[(crc^byte)&0xff];

	}

	return(crc^0xFFFFFFFF);

}
