/*
 * Copyright 2016 karawin (http://www.karawin.fr)
*/

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#include <string.h>
#include "interface.h"
#include "webserver.h"
#include "serv-fs.h"
#include "servers.h"
#include "driver/timer.h"
#include "driver/uart.h"
#include "audio_renderer.h"
#include "app_main.h"
#include "ota.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "webclient.h"
#include "vs1053.h"
#include "eeprom.h"
#include "interface.h"
#include "addon.h"

#include "lwip/opt.h"
#include "lwip/arch.h"
#include "lwip/api.h"
#include "lwip/sockets.h"


#define TAG "webserver"

xSemaphoreHandle semfile = NULL ;

const char strsROK[]  =  {"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n%s"};
const char tryagain[] = {"try again"};
const char strsJSON[] = {"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n%s"};

const char lowmemory[]  = { "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nlow memory\n"};
const char strsMALLOC[]  = {"WebServer inmalloc fails for %d\n"};
const char strsMALLOC1[]  = {"WebServer %s malloc fails\n"};
const char strsSOCKET[]  = {"WebServer Socket fails %s errno: %d\n"};
const char strsID[]  = {"getstation, no id or Wrong id %d\n"};
const char strsR13[]  = {"HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:13\r\n\r\n{\"%s\":\"%c\"}"};
// const char strsICY[]  = {"HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:%d\r\n\r\n{\"curst\":\"%s\",\"descr\":\"%s\",\"name\":\"%s\",\"bitr\":\"%s\",\"url1\":\"%s\",\"not1\":\"%s\",\"not2\":\"%s\",\"genre\":\"%s\",\"meta\":\"%s\",\"vol\":\"%s\",\"treb\":\"%s\",\"bass\":\"%s\",\"tfreq\":\"%s\",\"bfreq\":\"%s\",\"spac\":\"%s\",\"auto\":\"%c\"}"};
const char jsonICY[]  = {"{\
\"curst\":%u,\"descr\":\"%s\",\"name\":\"%s\",\"bitr\":\"%s\",\"url1\":\"%s\",\"not1\":\"%s\",\"not2\":\"%s\",\"genre\":\"%s\",\"meta\":\"%s\",\
\"vol\":%u,\"treb\":%d,\"bass\":%d,\"tfreq\":%d,\"bfreq\":%d,\"spac\":%d,\"auto\":%d}\
"};
const char jsonVS1053[]  = {"{\"treb\":%d,\"bass\":%d,\"tfreq\":%d,\"bfreq\":%d,\"spac\":%d}"};
const char strsWIFI[]  = {"HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:%d\r\n\r\n{\"ssid\":\"%s\",\"pasw\":\"%s\",\"ssid2\":\"%s\",\"pasw2\":\"%s\",\
\"ip\":\"%s\",\"msk\":\"%s\",\"gw\":\"%s\",\"ip2\":\"%s\",\"msk2\":\"%s\",\"gw2\":\"%s\",\"ua\":\"%s\",\"dhcp\":\"%s\",\"dhcp2\":\"%s\",\"mac\":\"%s\"\
,\"host\":\"%s\",\"tzo\":\"%s\"}"};
// const char strsGSTAT[]  = {"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n{\"Name\":\"%s\",\"URL\":\"%s\",\"File\":\"%s\",\"Port\":\"%d\",\"ovol\":\"%d\"}"};
const char jsonGSTAT[] = {"{\"station\":%u,\"Name\":\"%s\",\"URL\":\"%s\",\"File\":\"%s\",\"Port\":%u,\"ovol\":%u}"};
const char jsonNEW_STAT[] = {"{\"newstation\":%u,\"Name\":\"%s\",\"URL\":\"%s\",\"File\":\"%s\",\"Port\":%u,\"ovol\":%u}"};
static int8_t clientOvol = 0;


void *inmalloc(size_t n)
{
	void* ret;
//	ESP_LOGV(TAG, "server Malloc of %d %d,  Heap size: %d",n,((n / 32) + 1) * 32,xPortGetFreeHeapSize( ));
	ret = malloc(n);
	ESP_LOGV(TAG,"server Malloc of %x : %d bytes Heap size: %d",(int)ret,n,xPortGetFreeHeapSize( ));
//	if (n <4) printf("Server: incmalloc size:%d\n",n);
	return ret;
}
void infree(void *p)
{
	ESP_LOGV(TAG,"server free of   %x,            Heap size: %d",(int)p,xPortGetFreeHeapSize( ));
	if (p != NULL)free(p);
}

static struct servFile* findFile(char* name)
{
	struct servFile* f = (struct servFile*)&indexFile;
	while(1)
	{
		if(strcmp(f->name, name) == 0) return f;
		else f = f->next;
		if(f == NULL) return NULL;
	}
}

static void respOk(int conn,const char* message)
{
	char rempty[] = {""};
	if (message == NULL) message = rempty;
	char fresp[strlen(strsROK)+strlen(message)+15]; // = inmalloc(strlen(strsROK)+strlen(message)+15);
	sprintf(fresp,strsROK,"text/plain",strlen(message),message);
	ESP_LOGV(TAG,"respOk %s",fresp);
	write(conn, fresp, strlen(fresp));
}

static void respKo(int conn)
{
	write(conn, lowmemory, strlen(lowmemory));
}

static void respJSON(int conn,const char* message) {
	if (message == NULL || strlen(message) == 0) return;
	char fresp[strlen(strsJSON) + strlen(message) + 15]; // = inmalloc(strlen(strsROK)+strlen(message)+15);
	sprintf(fresp, strsJSON, strlen(message), message);
	ESP_LOGV(TAG, "respJSON %s", fresp);
	write(conn, fresp, strlen(fresp));
}

static void serveFile(char* name, int conn)
{
#define PART 1460
	int length = 0;
	int progress,part,gpart;
	char buf[270] = "HTTP/1.1 404 File not found\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: 158\r\n\r\n<!DOCTYPE html><html><head><title>404 Not Found</title></head><body><h1>Not Found</h1><p>The requested URL was not found on this server.</p></body></html>\r\n";
	char *content;
	if (strcmp(name,"/style.css") == 0)
	{
			if (g_device->options & T_THEME) strcpy(name , "/style1.css");
//			printf("name: %s, theme:%d\n",name,g_device->options&T_THEME);
	}
	struct servFile* f = findFile(name);
	ESP_LOGV(TAG,"find %s at %x",name,(int)f);
	ESP_LOGV(TAG,"Heap size: %d",xPortGetFreeHeapSize( ));
	gpart = PART;
	if(f != NULL)
	{
		length = f->size;
		content = (char*)f->content;
		progress = 0;
	}

	if(length > 0)
	{
		if (xSemaphoreTake(semfile,portMAX_DELAY ))
		{

			ESP_LOGV(TAG,"serveFile socket:%d,  %s. Length: %d  sliced in %d",conn,name,length,gpart);
			sprintf(buf, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n", (f!=NULL ? f->type : "text/plain"), length);
			ESP_LOGV(TAG,"serveFile send %d bytes\n%s",strlen(buf),buf);
			vTaskDelay(1); // why i need it? Don't know.
			if (write(conn, buf, strlen(buf)) == -1)
					 {respKo(conn); ESP_LOGE(TAG,"semfile fails 0 errno:%d",errno);xSemaphoreGive(semfile);	return;}
			progress = length;
			part = gpart;
			if (progress <= part) part = progress;
			while (progress > 0)
			{
				if (write(conn, content, part) == -1)
					  {respKo(conn); ESP_LOGE(TAG,"semfile fails 1 errno:%d",errno);xSemaphoreGive(semfile);	return;}

//				ESP_LOGV(TAG,"serveFile socket:%d,  read at %x len: %d",conn,(int)content,(int)part);
				content += part;
				progress -= part;
				if (progress <= part) part = progress;
				vTaskDelay(1);
			}
			xSemaphoreGive(semfile);
		} else {
			respKo(conn); ESP_LOGE(TAG,"semfile fails 2 errno:%d",errno);
			xSemaphoreGive(semfile);
		}
		return;
	}

	ESP_LOGV(TAG,"File not found : %s",name);
	if(write(conn, buf, strlen(buf)) == -1) {
		respKo(conn);
		ESP_LOGE(TAG,"semfile fails 0 errno:%d",errno);
		xSemaphoreGive(semfile);
	}

//	ESP_LOGV(TAG,"serveFile socket:%d, end",conn);
}

static bool getSParameter(char* result,uint32_t len,const char* sep,const char* param, char* data, uint16_t data_length) {
	if ((data == NULL) || (param == NULL))return false;
	char* p = strstr(data, param);
	if(p != NULL) {
		p += strlen(param);
		char* p_end = strstr(p, sep);
		if(p_end ==NULL) p_end = data_length + data;
		if(p_end != NULL ) {
			if (p_end==p) return false;
			if (len > (p_end-p )) len = p_end-p ;
			for(int i=0; i<len; i++) result[i] = 0;
			strncpy(result, p, len);
			result[len]=0;
			ESP_LOGV(TAG,"getSParam: in: \"%s\"   \"%s\"",data,result);
			return true;
		}
	}
	return false;
}

static char* getParameter(const char* sep,const char* param, char* data, uint16_t data_length) {
	if ((data == NULL) || (param == NULL))return NULL;
	char* p = strstr(data, param);
	if(p != NULL) {
		p += strlen(param);
		char* p_end = strstr(p, sep);
		if(p_end ==NULL) p_end = data_length + data;
		if(p_end != NULL ) {
			if (p_end==p) return NULL;
			char* t = inmalloc(p_end-p + 1);
			if (t == NULL) { printf("getParameterF fails\n"); return NULL;}
			ESP_LOGV(TAG,"getParameter malloc of %d  for %s",p_end-p + 1,param);
			int i;
			for(i=0; i<(p_end-p + 1); i++) t[i] = 0;
			strncpy(t, p, p_end-p);
			ESP_LOGV(TAG,"getParam: in: \"%s\"   \"%s\"",data,t);
			return t;
		} else return NULL;
	} else return NULL;
}

static char* getParameterFromResponse(const char* param, char* data, uint16_t data_length) {
	return getParameter("&",param,data, data_length) ;
}
static bool getSParameterFromResponse(char* result,uint32_t size, const char* param, char* data, uint16_t data_length) {
	return getSParameter(result,size,"&",param,data, data_length) ;
}
static char* getParameterFromComment(const char* param, char* data, uint16_t data_length) {
	return getParameter("\"",param,data, data_length) ;
}

// https://circuits4you.com/2019/03/21/esp8266-url-encode-decode-example/
unsigned char h2int(char c) {
	if (c >= '0' && c <='9'){
		return((unsigned char)c - '0');
	}
	if (c >= 'a' && c <='f'){
		return((unsigned char)c - 'a' + 10);
	}
	if (c >= 'A' && c <='F'){
		return((unsigned char)c - 'A' + 10);
	}
	return(0);
}

// decode URI like Javascript
static void pathParse(char* str) {
	char c;
	char code0;
	char code1;
	int j=0;
	if (str == NULL) return;
	for (int i=0; i< strlen(str);i++) {
		if(str[i] == '+') {
			c = ' ';
		} else if(str[i] == '%') {
			i++;
			code0 = str[i];
			i++;
			code1 = str[i];
			c = (h2int(code0) << 4) | h2int(code1);
		} else {
			c = str[i];
		}
		str[j] = c;
		j++;
	}
	str[j] = 0;
}

// volume offset
static void clientSetOvol(int8_t ovol)
{
	clientOvol = ovol;
	kprintf("##CLI.OVOLSET#: %d\n",ovol);
	vTaskDelay(10);
}

// set the volume with vol,  add offset
void setVolumei(int16_t vol) {
	vol += clientOvol;
	if (vol > 254) vol = 254;
	if (vol <0) vol = 1;
	if (get_audio_output_mode() == VS1053) VS1053_SetVolume(vol);
	if (vol <3) vol--;
	renderer_volume(vol+2); // max 256
}
void setVolume(char* vol) {
	int16_t uvol = atoi(vol);
	setIvol(uvol);
	uvol += clientOvol;
	if (uvol > 254) uvol = 254;
	if (uvol <0) uvol = 1;
	if(vol!= NULL) {
		if (get_audio_output_mode() == VS1053) VS1053_SetVolume(uvol);
		if (uvol <3) uvol--;
		renderer_volume(uvol+2); // max 256
		kprintf("##CLI.VOL#: %d\n",getIvol());
	}
}
// set the current volume with its offset
static void setOffsetVolume(void) {
	int16_t uvol = getIvol();
	uvol += clientOvol;
	if (uvol > 254) uvol = 254;
	if (uvol <=0) uvol = 1;
	ESP_LOGV(TAG,"setOffsetVol: %d",clientOvol);
	kprintf("##CLI.VOL#: %d\n",getIvol());
	setVolumei(uvol);
}



uint16_t getVolume() {
	return (getIvol());
}

// Set the volume with increment vol
void setRelVolume(int8_t vol) {
	char Vol[5];
	int16_t rvol;
	rvol = getIvol()+vol;
	if (rvol <0) rvol = 0;
	if (rvol > 254) rvol = 254;
	sprintf(Vol,"%d",rvol);
	setVolume(Vol);
	wsVol(Vol);
}


// send the rssi
static void rssi(int socket) {
	char answer[20];
	int8_t rssi = -30;
	wifi_ap_record_t wifidata;
	esp_wifi_sta_get_ap_info(&wifidata);
	if (wifidata.primary != 0) {
		rssi = wifidata.rssi;
	}
	sprintf(answer,"{\"wsrssi\":\"%d\"}",rssi);
	websocketwrite(socket,answer, strlen(answer));
}


// flip flop the theme indicator
static void theme() {
	if ((g_device->options&T_THEME)!=0) g_device->options&=NT_THEME; else g_device->options |= T_THEME;
	saveDeviceSettings(g_device);
	ESP_LOGV(TAG,"theme:%d",g_device->options&T_THEME);
}

// treat the received message of the websocket
void websockethandle(int socket, wsopcode_t opcode, uint8_t * payload, size_t length) {
	// Commands to add : volume, setStation, hardware, updateOk, erase, timerOn, timerOff, sound, auto, ...
	ESP_LOGV(TAG,"websocketHandle: %s", payload);
	char noStation[73+76+6] = {"{\"Name\":\"\",\"URL\":\"\",\"File\":\"\",\"Port\":0,\"ovol\":0}"};
	char value[6] = "";

	if(strcmp((char*)payload,"wsrssi") == 0) {
		rssi(socket);
		return;
	} else if(strstr((char*)payload,"wsvol=")!= NULL) {
		char answer[17];
		if (strstr((char*)payload,"&") != NULL)
			*strstr((char*)payload,"&")=0;
		else return;
//		setVolume(payload+6);
		sprintf(answer,"{\"wsvol\":\"%s\"}",payload+6);
		websocketlimitedbroadcast(socket,answer, strlen(answer));
	} else if(getSParameterFromResponse(value, sizeof(value), "play=", (char*)payload, length)) {
		int uid = atoi(value);
		if (uid >=0 && uid < 255) {
			playStationInt(uid);
		}
		return;
	} else if(getSParameterFromResponse(value, sizeof(value), "getStationsFrom=", (char*)payload, length)) {
		int uid = atoi(value);
		if (uid >=0 && uid < 255) {
			int max_length = 1024;
			char *output;
			output = inmalloc(max_length);
			if (output == NULL) {
				ESP_LOGE(TAG," %s malloc fails (%d bytes)","getStation", max_length);
			} else {
				/*
					struct shoutcast_info {
						char domain[73]; //url
						char file[116];  //path
						char name[64];
						int8_t ovol; // offset volume
						uint16_t port;	//port
					};
				 * */
				int json_length = 335; // 73 + 116 + 64 + 8 + 16 + 68 - 12 + 2
				char *buf;
				buf = inmalloc(json_length);
				if(buf == NULL) {
					ESP_LOGE(TAG," %s malloc fails (%d bytes)", "getStation", json_length);
				} else {
					strcpy(output, "{\"stations\":[");
					bool not_first = false;
					bool isFull = false;
					for(int uid_cc=uid, sta_max=255; uid_cc<sta_max; uid_cc++) {
						struct shoutcast_info* si = getStation(uid_cc); // getStation uses malloc() !!!!
						if(si != NULL && strlen(si->domain) > 0) {
							if (strlen(si->domain) > sizeof(si->domain)) si->domain[sizeof(si->domain)-1] = 0; //truncate if any (rom crash)
							if (strlen(si->file) > sizeof(si->file)) si->file[sizeof(si->file)-1] = 0; //truncate if any (rom crash)
							if (strlen(si->name) > sizeof(si->name)) si->name[sizeof(si->name)-1] = 0; //truncate if any (rom crash)
							// jsonGSTAT: {"station":%u,"Name":"%s","URL":"%s","File":"%s","Port":%u,"ovol":%u};
							sprintf(buf, "{\"station\":%u,\"Name\":\"%s\",\"URL\":\"%s\",\"File\":\"%s\"",
								uid_cc,
								si->name,
								si->domain,
								si->file
							);
							char uval[12];
							if(si->port != 80 && si->port != 0) {
								sprintf(uval, ",\"Port\":%u", si->port);
								strcat(buf, uval);
							}
							if(si->ovol > 0) {
								sprintf(uval, ",\"ovol\":%u", si->ovol);
								strcat(buf, uval);
							}
							strcat(buf, "}");

							isFull = (strlen(output) + strlen(buf) >= max_length - 10);
							if(!isFull) {
								if(not_first) {
									strcat(output, ",");
								}
								strcat(output, buf);
								not_first = true;
							}
						}
						infree(si);
						if(isFull) { break; }
					}

					infree(buf);

					strcat(output, "]}");
					ESP_LOGV(TAG,"getStation Buf len:%d : %s", strlen(output), output);
					websocketwrite(socket, output, strlen(output));
				}
				infree(output);
			}
		}
		return;
	} else if(getSParameterFromResponse(value, sizeof(value), "getStation=", (char*)payload, length)) {
		int uid = atoi(value);
		if (uid >=0 && uid < 255) {
			struct shoutcast_info* si = getStation(uid);
			if(si == NULL) {
				// A response must always sent for getting the next station
				websocketwrite(socket, noStation, strlen(noStation));
			} else {
				if (strlen(si->domain) > sizeof(si->domain)) si->domain[sizeof(si->domain)-1] = 0; //truncate if any (rom crash)
				if (strlen(si->file) > sizeof(si->file)) si->file[sizeof(si->file)-1] = 0; //truncate if any (rom crash)
				if (strlen(si->name) > sizeof(si->name)) si->name[sizeof(si->name)-1] = 0; //truncate if any (rom crash)
				// jsonGSTAT: {"station":%u,"Name":"%s","URL":"%s","File":"%s","Port":%u,"ovol":%u}
				int json_length = 12 + strlen(jsonGSTAT) + strlen(si->domain) + strlen(si->file) + strlen(si->name);
				char *buf;
				buf = inmalloc(json_length);
				if (buf == NULL) {
					ESP_LOGE(TAG," %s malloc fails (%d bytes)","getStation", json_length);
					websocketwrite(socket, noStation, strlen(noStation));
				} else {
					// for(int i = 0; i<sizeof(buf); i++) buf[i] = 0;
					sprintf(buf, jsonGSTAT,
						uid,
						si->name,
						si->domain,
						si->file,
						si->port,
						si->ovol
					);
					ESP_LOGV(TAG,"getStation Buf len:%d : %s",strlen(buf),buf);
					// A response must always sent for getting the next station
					websocketwrite(socket, buf, strlen(buf));
					infree(buf);
				}
				infree(si);
			}
		}
		return;
	} else if(strstr((char*)payload,"newStationName=") != NULL && strstr((char*)payload,"newStationUrl=") != NULL) {
		struct shoutcast_info* si;
		for(uint16_t i=0; i<255; i++) {
			si = getStation(i); // getStation uses malloc() !!!!
			if(si != NULL && strlen(si->domain) == 0) {
				char* url = getParameterFromResponse("newStationUrl=", (char *)payload, length);
				char scheme[6] = "\0";
				char domain[73] = "\0";
				char path1[116] = "/";
				uint port = 80;
				if(
					sscanf(url, "%5[a-z]://%73[^:]:%d%115s", scheme, domain, &port, path1) == 4 ||
					sscanf(url, "%5[a-z]://%73[^/]%115s", scheme, domain, path1) == 3 ||
					sscanf(url, "%5[a-z]://%73[^:]:%d", scheme, domain, &port) == 3 ||
					sscanf(url, "%5[a-z]://%73[^/:]", scheme, domain) == 2
				) {
					// checks scheme !!!
					strcpy(si->domain, domain);
					si->port = port;
					strcpy(si->file, path1);
					si->ovol = 0;
					char* name = getParameterFromResponse("newStationName=", (char *)payload, length);
					pathParse(name);
					if(strlen(name) > sizeof(si->name)) { // 64 bytes
						name[sizeof(si->name)-1] = 0;
					}
					strcpy(si->name, name);
					ESP_LOGI(TAG,"New station id: %u name: \"%s\" domain: %s port: %u path: %s", i, si->name, si->domain, si->port, si->file);
					saveStation(si, i);

					int json_length = 12 + strlen(jsonNEW_STAT) + strlen(si->domain) + strlen(si->file) + strlen(si->name);
					char *buf;
					buf = inmalloc(json_length);
					if (buf == NULL) {
						ESP_LOGE(TAG," %s malloc fails (%d bytes)","getStation", json_length);
						websocketwrite(socket, noStation, strlen(noStation));
					} else {
						// for(int i = 0; i<sizeof(buf); i++) buf[i] = 0;
						sprintf(buf, jsonNEW_STAT,
							i,
							si->name,
							si->domain,
							si->file,
							si->port,
							si->ovol
						);

						websocketwrite(socket, buf, strlen(buf));
						infree(buf);
					}
				} else {
					ESP_LOGW(TAG,"New station failed - id: %u - bad url: %s", i, url);
				}
				infree(si);
				break;
			}
			infree(si);
		}
		return;
	} else if(getSParameterFromResponse(noStation, sizeof(noStation), "instant=", (char*)payload, length)) {
		clientDisconnect("Web Instant");
		pathParse(noStation);
		clientParsePlaylist(noStation);
		clientSetName("Instant Play", 255);
		clientConnectOnce();
		vTaskDelay(1);
		return;
	} else if(strcmp((char*)payload, "stop") == 0) {
		clientDisconnect("Web stop");
		// wsicy message will be sent
		return;
	} else if(strcmp((char*)payload, "start") == 0) {
		playStationInt(getCurrentStation());
		// wsicy message will be sent
		return;
	} else if(strstr((char*)payload,"startSleep=")!= NULL) {
		if (strstr((char*)payload,"&") != NULL)
			*strstr((char*)payload,"&")=0;
		else return;
		startSleep(atoi((char*)payload+11));
	} else if(strstr((char*)payload,"stopSleep")!= NULL) {
		stopSleep();
	} else if (strstr((char*)payload,"startWake=")!= NULL) {
		if (strstr((char*)payload,"&") != NULL)
			*strstr((char*)payload,"&")=0;
		else return;
		startWake(atoi((char*)payload+10));
	} else if (strstr((char*)payload,"stopWake")!= NULL) {
		stopWake();
	} else if (strstr((char*)payload,"monitor")!= NULL) {
		wsMonitor();
	} else if (strstr((char*)payload,"theme")!= NULL) {
		theme();
	}
}

void playStationInt(int sid) {
	struct shoutcast_info* si = getStation(sid);
	if(si != NULL && si->domain && si->file) {
		vTaskDelay(4);
		clientSilentDisconnect();
		ESP_LOGV(TAG,"playstationInt: %d, name: %s, domain: %s, port: %u, file: %s", sid, si->name, si->domain, si->port, si->file);
		clientSetName(si->name, sid);
		clientSetURL(si->domain);
		clientSetPath(si->file);
		clientSetPort(si->port);
		clientSetOvol(si->ovol);
		infree(si);

		clientConnect();
		setOffsetVolume();
		for(int i=0; i<100; i++) {
			if (clientIsConnected()) break;
			vTaskDelay(5);
		}
		char answer[24];
		sprintf(answer,"{\"wsstation\":%u}", sid);
		websocketbroadcast(answer, strlen(answer));
		ESP_LOGI(TAG,"playstationInt: %u, g_device: %u", sid, g_device->currentstation);
		if (g_device->currentstation != sid) {
			g_device->currentstation = sid;
			setCurrentStation(sid);
			saveDeviceSettings(g_device);
		}
	} else {
		ESP_LOGW(TAG, "PlaystationInt %u not found", sid);
	}
}

void playStation(char* id) {
	int uid = atoi(id) ;
	ESP_LOGV(TAG,"playstation: %d",uid);
	if (uid < 255)
		setCurrentStation(uid) ;
	playStationInt(getCurrentStation());
}

static void handlePOST(char* name, char* data, int data_size, int conn) {
	ESP_LOGD(TAG,"HandlePost %s\n",name);
	bool changed = false;
	if(strcmp(name, "/instant_play") == 0) {
		if(data_size > 0) {
			bool tst;
			char url[100];
			tst = getSParameterFromResponse(url,100,"url=", data, data_size);
			char path[200];
			tst &=getSParameterFromResponse(path,200,"path=", data, data_size);
			pathParse(path);
			char port[10];
			tst &=getSParameterFromResponse(port,10,"port=", data, data_size);
			if(tst) {
				clientDisconnect("Post instant_play");
				for (int i = 0; i<100; i++) {
					if(!clientIsConnected())break;
					vTaskDelay(4);
				}
				clientSetURL(url);
				clientSetPath(path);
				clientSetPort(atoi(port));
				clientSetOvol(0);
				clientConnectOnce();
				setOffsetVolume();
				for (int i = 0; i<100; i++) {
					if (clientIsConnected()) break;
					vTaskDelay(5);
				}
			}
		}
	} else if(strcmp(name, "/soundvol") == 0) {
		if(data_size > 0) {
			char param[4];
			int vol;
			if(getSParameterFromResponse(param,4,"vol=", data, data_size)) {
				if(param == NULL) { return; }
				vol = atoi(param);
				if(vol < 0 || vol > 254) { return; }
				ESP_LOGD(TAG,"/soundvol vol: %s num:%d", param, vol);
				setVolume(param); // setVolume waits for a string
				wsVol(param);
				respOk(conn,NULL);
			}
		}
		return;
	} else if(strcmp(name, "/sound") == 0) {
		if(data_size > 0) {
			char value[6];
			changed = false;
			if (get_audio_output_mode() == VS1053) {
				if(getSParameterFromResponse(value, 6, "bass=", data, data_size)) {
					int uVal = atoi(value);
					if(g_device->bass != uVal) {
						VS1053_SetBass(uVal);
						g_device->bass = uVal;
						changed = true;
					}
				}
				if(getSParameterFromResponse(value, 6, "treble=", data, data_size)) {
					int uVal = atoi(value);
					if (g_device->treble != uVal) {
						VS1053_SetTreble(uVal);
						g_device->treble = uVal;
						changed = true;
					}
				}
				if(getSParameterFromResponse(value, 6, "bassfreq=", data, data_size)) {
					int uVal = atoi(value);
					if (g_device->freqbass != uVal) {
						VS1053_SetBassFreq(uVal);
						g_device->freqbass = uVal;
						changed = true;
					}
				}
				if(getSParameterFromResponse(value, 6, "treblefreq=", data, data_size)) {
					int uVal = atoi(value);
					if (g_device->freqtreble != uVal) {
						VS1053_SetTrebleFreq(uVal);
						g_device->freqtreble = uVal;
						changed = true;
					}
				}
				if(getSParameterFromResponse(value, 6, "spacial=", data, data_size)) {
					int uVal = atoi(value);
					if (g_device->spacial != uVal) {
						VS1053_SetSpatial(uVal);
						g_device->spacial = uVal;
						changed = true;
					}
				}
				if(changed) {
					saveDeviceSettings(g_device);
					// jsonVS1053: {"treb":%d,"bass":%d,"tfreq":%d,"bfreq":%d,"spac":%d}
					int json_length = 52;
					char *buf;
					buf = inmalloc(json_length);
					sprintf(buf, jsonVS1053,
						VS1053_GetTreble(),
						VS1053_GetBass(),
						VS1053_GetTrebleFreq(),
						VS1053_GetBassFreq(),
						VS1053_GetSpatial()
					);
					respJSON(conn, buf);
					infree(buf);
				}
				return;
			}
		}
	} else if(strcmp(name, "/getStation") == 0) {
		if(data_size > 0) {
			char id[6];
			if (getSParameterFromResponse(id,6,"idgp=", data, data_size) ) {
				int uid = atoi(id);
				if (uid >=0 && uid < 255) {
					char *buf;
					struct shoutcast_info* si = getStation(uid);
					if (strlen(si->domain) > sizeof(si->domain)) si->domain[sizeof(si->domain)-1] = 0; //truncate if any (rom crash)
					if (strlen(si->file) > sizeof(si->file)) si->file[sizeof(si->file)-1] = 0; //truncate if any (rom crash)
					if (strlen(si->name) > sizeof(si->name)) si->name[sizeof(si->name)-1] = 0; //truncate if any (rom crash)
					// jsonGSTAT: {"station":%u,"Name":"%s","URL":"%s","File":"%s","Port":%u,"ovol":%u}
					int json_length =  - 2 * 5 + 8 + 1 + strlen(jsonGSTAT) + strlen(si->domain) + strlen(si->file) + strlen(si->name);
					buf = inmalloc(json_length);
					if (buf == NULL) {
						ESP_LOGE(TAG," %s malloc fails (%d bytes)","getStation", json_length);
						respKo(conn);
						//return;
					} else {
						for(int i = 0; i<sizeof(buf); i++) buf[i] = 0;
						sprintf(buf, jsonGSTAT,
							uid,
							si->name,
							si->domain,
							si->file,
							si->port,
							si->ovol
						);
						ESP_LOGV(TAG,"getStation Buf len:%d : %s",strlen(buf),buf);
						respJSON(conn, buf);
					}
					infree(buf);
					infree(si);
					return;
				} else {
					printf(strsID,atoi(id));
				}
//				infree (id);
			}
		}
	} else if(strcmp(name, "/setStation") == 0) {
		if(data_size > 0) {
//printf("data:%s\n",data);
			char nb[6] ;
			bool res;
			uint16_t unb,uid = 0;
			bool pState = getState();  // remember if we are playing
			res=getSParameterFromResponse(nb,6,"nb=", data, data_size);
			if (res)
			{	ESP_LOGV(TAG,"Setstation: nb init:%s",nb);
				unb = atoi(nb);
			}
			else
			{
				unb = 1;
				ESP_LOGE(TAG," %s nb null > set to 1 by default","setStation");
			}

			ESP_LOGV(TAG,"unb init:%d",unb);
			struct shoutcast_info *si =  inmalloc(sizeof(struct shoutcast_info)*unb);
			struct shoutcast_info *nsi ;

			if (si == NULL) {
				ESP_LOGE(TAG," %s malloc fails", "setStation");
				respKo(conn);
				return;
			}
			char* bsi = (char*)si;
			for (int j=0;j< sizeof(struct shoutcast_info)*unb;j++) bsi[j]=0; //clean

			char* url; char* file; char* name;
			for(int i=0; i<unb; i++) {
				nsi = si + i;
				char id[6] = "";
				if(getSParameterFromResponse(id,6,"id=", data, data_size) && strlen(id) > 0) {
					int idNum = atoi(id);
					ESP_LOGV(TAG,"nb:%d, id:%s", i, id);
					if (idNum >=0 && idNum < 255) {
						if (i == 0) uid = idNum;
						url = getParameterFromResponse("url=", data, data_size);
						file = getParameterFromResponse("file=", data, data_size);
						name = getParameterFromResponse("name=", data, data_size);
						char port[6] = "80";
						if(url && file && name && getSParameterFromResponse(port,6,"port=", data, data_size)) {
							if (strlen(url) >= sizeof(nsi->domain)) url[sizeof(nsi->domain)-1] = 0; //truncate if any
							strcpy(nsi->domain, url);
							pathParse(file);
							if (strlen(file) >= sizeof(nsi->file)) url[sizeof(nsi->file)-1] = 0; //truncate if any
							strcpy(nsi->file, file);
							pathParse(name);
							if (strlen(name) >= sizeof(nsi->name)) url[sizeof(nsi->name)-1] = 0; //truncate if any
							strcpy(nsi->name, name);
							char ovol[6] = "";
							nsi->ovol = (getSParameterFromResponse(ovol,6,"ovol=", data, data_size) && strlen(ovol) > 0) ? atoi(ovol) : 0;
							nsi->port = (strlen(port) > 0) ? atoi(port) : 80;
//							ESP_LOGW(TAG,"nb:%d,si:%x,nsi:%x,id:%s,url:%s,file:%s",i,(int)si,(int)nsi,id,url,file);
							ESP_LOGV(TAG,"id:%3d => url:%s, port:%d, file:%s", idNum, nsi->domain, nsi->port, nsi->file);
						}
						infree(name);
						infree(file);
						infree(url);
					}
				}

				data = strstr(data,"&&")+2;
				ESP_LOGV(TAG,"si:%x, nsi:%x, addr:%x",(int)si,(int)nsi,(int)data);
			}
			ESP_LOGV(TAG,"save station: %d, unb:%d, addr:%x",uid,unb,(int)si);
			saveMultiStation(si, uid,unb);
			ESP_LOGV(TAG,"save station return: %d, unb:%d, addr:%x",uid,unb,(int)si);
			infree (si);
			if (pState != getState())
				if (pState) {clientConnect();vTaskDelay(200);}	 //we was playing so start again the play
		}
	} else if(strcmp(name, "/play") == 0) {
		char param[4];
		if(getSParameterFromResponse(param,4,"id=", data, data_size)) {
			playStation(param);
		}
	} else if(strcmp(name, "/auto") == 0) {
		if(data_size > 4) {
			char * id = data+3;
			data[data_size-1] = 0;
			if ((strcmp(id,"true"))&&(g_device->autostart==1)) {
				g_device->autostart = 0;
				ESP_LOGV(TAG,"autostart: %s, num:%d",id,g_device->autostart);
				saveDeviceSettings(g_device);
			}
			else
			if ((strcmp(id,"false"))&&(g_device->autostart==0))
			{
				g_device->autostart = 1;
				ESP_LOGV(TAG,"autostart: %s, num:%d",id,g_device->autostart);
				saveDeviceSettings(g_device);
			}
		}
	} else if(strcmp(name, "/rauto") == 0) {
		char buf[strlen(strsR13)+16];// = inmalloc( strlen(strsRAUTO)+16);
		sprintf(buf, strsR13,"rauto",(g_device->autostart)?'1':'0' );
		write(conn, buf, strlen(buf));
		return;

	} else if(strcmp(name, "/theme") == 0) {
		char buf[strlen(strsR13)+16];// = inmalloc( strlen(strsRAUTO)+16);
		sprintf(buf, strsR13,"theme",(g_device->options & T_THEME)?'1':'0' );
		write(conn, buf, strlen(buf));
		return;

	} else if(strcmp(name, "/stop") == 0) {
		if (clientIsConnected())
		{
			clientDisconnect("Post Stop");
			for (int i = 0;i<100;i++)
			{
				if (!clientIsConnected()) break;
				vTaskDelay(4);
			}
		}
	} else if(strcmp(name, "/upgrade") == 0) {
		update_firmware((char*)"KaRadio32");  // start the OTA
	} else if(strcmp(name, "/icy") == 0) {
		ESP_LOGV(TAG,"icy vol");
		bool isVS1053 = (get_audio_output_mode() == VS1053);
		struct icyHeader *header = clientGetHeader();
		ESP_LOGV(TAG,"icy start header %x",(int)header);
		char* not2 = (header->members.single.notice2 != NULL) ? header->members.single.notice2 : header->members.single.audioinfo;
		//if ((header->members.single.notice2 != NULL)&&(strlen(header->members.single.notice2)==0)) not2=header->members.single.audioinfo;
		/* jsonICY: {
		 *	"curst":%d,"descr":"%s","name":"%s","bitr":"%s","url1":"%s","not1":"%s","not2":"%s",
		 *	"genre":"%s","meta":"%s",
		 *	"vol":%d,"treb":%d,"bass":%d,"tfreq":%d,"bfreq":%d,"spac":%d,"auto":%d
		 * }
		 * */
		/* jsonICY: {"curst":,"descr":"","name":"","bitr":"","url1":"","not1":"","not2":"","genre":"","meta":"","vol":,"treb":,"bass":,"tfreq":,"bfreq":,"spac":,"auto":%d} */
		int json_length = 175; // json_length += strlen(currentSt) + strlen(vol) + strlen(treble) + strlen(bass) + strlen(tfreq) + strlen(bfreq) + strlen(spac);
		if(header->members.single.description != NULL) { json_length += strlen(header->members.single.description); }
		if(header->members.single.name != NULL) { json_length += strlen(header->members.single.name); }
		if(header->members.single.bitrate != NULL) { json_length += strlen(header->members.single.bitrate); }
		if(header->members.single.url != NULL) { json_length += strlen(header->members.single.url); }
		if(header->members.single.notice1 != NULL) { json_length += strlen(header->members.single.notice1); }
		if(not2 != NULL) { json_length += strlen(not2); }
		if(header->members.single.genre != NULL) { json_length += strlen(header->members.single.genre); }
		if(header->members.single.metadata != NULL) { json_length += strlen(header->members.single.metadata); }
		ESP_LOGD(TAG,"icy start header %x  len:%d - vol:%d", (int)header, json_length, getVolume());

		char *buf = inmalloc( json_length);
		if (buf == NULL) {
			ESP_LOGE(TAG," %s malloc fails","post icy");
			infree(buf);
			respKo(conn);
			return;
		} else {
			sprintf(buf, jsonICY,
				getCurrentStation(),
				(header->members.single.description != NULL) ? header->members.single.description : "",
				(header->members.single.name != NULL) ? header->members.single.name : "",
				(header->members.single.bitrate != NULL) ? header->members.single.bitrate : "",
				(header->members.single.url != NULL) ? header->members.single.url : "",
				(header->members.single.notice1 != NULL) ? header->members.single.notice1 : "",
				(not2 != NULL) ? not2 : "",
				(header->members.single.genre != NULL) ? header->members.single.genre : "",
				(header->members.single.metadata != NULL) ? header->members.single.metadata : "",
				getVolume(),
				(isVS1053) ? VS1053_GetTreble() : 0,
				(isVS1053) ? VS1053_GetBass() : 0,
				(isVS1053) ? VS1053_GetTrebleFreq() : 0,
				(isVS1053) ? VS1053_GetBassFreq() : 0,
				(isVS1053) ? VS1053_GetSpatial() : 0,
				(g_device->autostart) ? 1 : 0
			);
			ESP_LOGV(TAG,"test: len fmt:%d %d\n%s\n", strlen(buf), strlen(jsonICY), buf);
			respJSON(conn, buf);
			infree(buf);
			wsMonitor();
			return;
		}
	} else if(strcmp(name, "/hardware") == 0) {
		bool val = false;
		uint8_t cout;
		changed = false;
		if(data_size > 0) {
			char valid[6];
			if(getSParameterFromResponse(valid,6,"valid=", data, data_size))
				if (strcmp(valid,"1")==0) val = true;
			char coutput[6];
			getSParameterFromResponse(coutput,6,"coutput=", data, data_size);
			cout = atoi(coutput);
			if (val)
			{
				g_device->audio_output_mode = cout;
				changed = true;
				saveDeviceSettings(g_device);
			}
			int json_length ;
			json_length =15;

			char buf[110];
			sprintf(buf, "HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:%d\r\n\r\n{\"coutput\":\"%d\"}",
			json_length,
			g_device->audio_output_mode);
			ESP_LOGV(TAG,"hardware Buf len:%d\n%s",strlen(buf),buf);
			write(conn, buf, strlen(buf));
			if (val){
				// set current_ap to the first filled ssid
				ESP_LOGD(TAG,"audio_output_mode: %d",g_device->audio_output_mode);
//				copyDeviceSettings();
				vTaskDelay(20);
				esp_restart();
			}
			return;
		}
	} else if(strcmp(name, "/wifi") == 0) {
		bool val = false;
		char tmpip[16],tmpmsk[16],tmpgw[16];
		char tmpip2[16],tmpmsk2[16],tmpgw2[16],tmptzo[10];
		changed = false;
		if(data_size > 0) {
			char valid[5];
			if(getSParameterFromResponse(valid,5,"valid=", data, data_size))
				if (strcmp(valid,"1")==0) val = true;
			char* aua = getParameterFromResponse("ua=", data, data_size);
			pathParse(aua);
			char* host = getParameterFromResponse("host=", data, data_size);
			pathParse(host);
			char* tzo = getParameterFromResponse("tzo=", data, data_size);
			pathParse(tzo);


//			ESP_LOGV(TAG,"wifi received  valid:%s,val:%d, ssid:%s, pasw:%s, aip:%s, amsk:%s, agw:%s, adhcp:%s, aua:%s",valid,val,ssid,pasw,aip,amsk,agw,adhcp,aua);
			if (val) {
				char adhcp[4],adhcp2[4];
				char* ssid = getParameterFromResponse("ssid=", data, data_size);
				pathParse(ssid);
				char* pasw = getParameterFromResponse("pasw=", data, data_size);
				pathParse(pasw);
				char* ssid2 = getParameterFromResponse("ssid2=", data, data_size);
				pathParse(ssid2);
				char* pasw2 = getParameterFromResponse("pasw2=", data, data_size);
				pathParse(pasw2);
				char* aip = getParameterFromResponse("ip=", data, data_size);
				char* amsk = getParameterFromResponse("msk=", data, data_size);
				char* agw = getParameterFromResponse("gw=", data, data_size);
				char* aip2 = getParameterFromResponse("ip2=", data, data_size);
				char* amsk2 = getParameterFromResponse("msk2=", data, data_size);
				char* agw2 = getParameterFromResponse("gw2=", data, data_size);

				changed = true;
				ip_addr_t valu;
				if (aip != NULL)
				{
					ipaddr_aton(aip, &valu);
					memcpy(g_device->ipAddr1,&valu,sizeof(uint32_t));
					ipaddr_aton(amsk, &valu);
					memcpy(g_device->mask1,&valu,sizeof(uint32_t));
					ipaddr_aton(agw, &valu);
					memcpy(g_device->gate1,&valu,sizeof(uint32_t));
				}
				if (aip2 != NULL)
				{
					ipaddr_aton(aip2, &valu);
					memcpy(g_device->ipAddr2,&valu,sizeof(uint32_t));
					ipaddr_aton(amsk2, &valu);
					memcpy(g_device->mask2,&valu,sizeof(uint32_t));
					ipaddr_aton(agw2, &valu);
					memcpy(g_device->gate2,&valu,sizeof(uint32_t));
				}
				if (getSParameterFromResponse(adhcp,4,"dhcp=", data, data_size))
					if (strlen(adhcp)!=0)
					{if (strcmp(adhcp,"true")==0)
					g_device->dhcpEn1 = 1; else g_device->dhcpEn1 = 0;}
				if (getSParameterFromResponse(adhcp2,4,"dhcp2=", data, data_size))
					if (strlen(adhcp2)!=0)
					{if (strcmp(adhcp2,"true")==0)
					g_device->dhcpEn2 = 1; else g_device->dhcpEn2 = 0;}

				strcpy(g_device->ssid1,(ssid==NULL)?"":ssid);
				strcpy(g_device->pass1,(pasw==NULL)?"":pasw);
				strcpy(g_device->ssid2,(ssid2==NULL)?"":ssid2);
				strcpy(g_device->pass2,(pasw2==NULL)?"":pasw2);

				infree(ssid); infree(pasw);infree(ssid2); infree(pasw2);
				infree(aip);infree(amsk);infree(agw);
				infree(aip2);infree(amsk2);infree(agw2);
			}

			if ((g_device->ua!= NULL)&&(strlen(g_device->ua)==0))
			{
				if (aua==NULL) {aua= inmalloc(12); strcpy(aua,"Karadio/1.6");}
			}
			if (aua!=NULL)
			{
				if ((strcmp(g_device->ua,aua) != 0)&&(strcmp(aua,"undefined") != 0))
				{
					strcpy(g_device->ua,aua);
					changed = true;
				}
				infree(aua);
			}

			if (host!=NULL)
			{
				if (strlen(host) >0)
				{
					if ((strcmp(g_device->hostname,host) != 0)&&(strcmp(host,"undefined") != 0))
					{
						strncpy(g_device->hostname,host,HOSTLEN-1);
						setHostname(g_device->hostname);
						changed = true;
					}
				}
				infree(host);
			}

			if (tzo==NULL)
			{
				tzo= inmalloc(10);
				sprintf(tmptzo,"%d",g_device->tzoffset);
				strcpy(tzo,tmptzo);
			}
			else if (strlen(tzo) ==0)
			{
				free (tzo);
				tzo= inmalloc(10); strcpy(tzo,"0");
			}

			if (strlen(tzo) >0)
			{
				if (strcmp(tzo,"undefined") != 0)
				{
					g_device->tzoffset= atoi(tzo);
					addonDt();
					changed = true;
				}
			}
			infree(tzo);

			if (changed)
			{
				saveDeviceSettings(g_device);
			}
			uint8_t macaddr[10]; // = inmalloc(10*sizeof(uint8_t));
			char macstr[20]; // = inmalloc(20*sizeof(char));
			char adhcp[4],adhcp2[4];
			esp_wifi_get_mac(WIFI_IF_STA,macaddr);
			int json_length ;
			json_length =95+ 39+ 19+
			strlen(g_device->ssid1) +
			strlen(g_device->pass1) +
			strlen(g_device->ssid2) +
			strlen(g_device->pass2) +
			strlen(g_device->ua)+
			strlen(g_device->hostname)+
			sprintf(tmptzo,"%d",g_device->tzoffset)+
			sprintf(tmpip,"%d.%d.%d.%d",g_device->ipAddr1[0], g_device->ipAddr1[1],g_device->ipAddr1[2], g_device->ipAddr1[3])+
			sprintf(tmpmsk,"%d.%d.%d.%d",g_device->mask1[0], g_device->mask1[1],g_device->mask1[2], g_device->mask1[3])+
			sprintf(tmpgw,"%d.%d.%d.%d",g_device->gate1[0], g_device->gate1[1],g_device->gate1[2], g_device->gate1[3])+
			sprintf(adhcp,"%d",g_device->dhcpEn1)+
			sprintf(tmpip2,"%d.%d.%d.%d",g_device->ipAddr2[0], g_device->ipAddr2[1],g_device->ipAddr2[2], g_device->ipAddr2[3])+
			sprintf(tmpmsk2,"%d.%d.%d.%d",g_device->mask2[0], g_device->mask2[1],g_device->mask2[2], g_device->mask2[3])+
			sprintf(tmpgw2,"%d.%d.%d.%d",g_device->gate2[0], g_device->gate2[1],g_device->gate2[2], g_device->gate2[3])+
			sprintf(adhcp2,"%d",g_device->dhcpEn2)+
			sprintf(macstr,MACSTR,MAC2STR(macaddr)
			);

			char *buf = inmalloc( json_length + 95+39+10);
			if (buf == NULL) {
				ESP_LOGE(TAG," %s malloc fails","post wifi");
				respKo(conn);
				//return;
			} else {
				sprintf(buf, strsWIFI,
				json_length,
				g_device->ssid1,g_device->pass1,g_device->ssid2,g_device->pass2,tmpip,tmpmsk,tmpgw,tmpip2,tmpmsk2,tmpgw2,g_device->ua,adhcp,adhcp2,macstr,g_device->hostname,tmptzo);
				ESP_LOGV(TAG,"wifi Buf len:%d\n%s",strlen(buf),buf);
				write(conn, buf, strlen(buf));
				infree(buf);
			}

			if (val) {
				// set current_ap to the first filled ssid
				ESP_LOGD(TAG,"currentAP: %d",g_device->current_ap);
				if (g_device->current_ap == APMODE)
				{
					if (strlen(g_device->ssid1)!= 0) g_device->current_ap = STA1;
					else
					if (strlen(g_device->ssid2)!= 0) g_device->current_ap = STA2;
					saveDeviceSettings(g_device);
				}
				ESP_LOGD(TAG,"currentAP: %d",g_device->current_ap);
				copyDeviceSettings();	// save the current one
				vTaskDelay(50);
				esp_restart();
			}
			return;
		}
	} else if(strcmp(name, "/clear") == 0) {
		eeEraseStations();	//clear all stations
	}
	respOk(conn,NULL);
}

static bool httpServerHandleConnection(int conn, char* buf, uint16_t buflen) {
	char* c;
	char* d;
	ESP_LOGD(TAG,"Heap size: %d",xPortGetFreeHeapSize( ));
//printf("httpServerHandleConnection  %20c \n",&buf);
	if( (c = strstr(buf, "GET ")) != NULL) {
		ESP_LOGV(TAG,"GET socket:%d len: %d, str:\n%s",conn,buflen,buf);
		if( ((d = strstr(buf,"Connection:")) !=NULL)&& ((d = strstr(d," Upgrade")) != NULL))
		{  // a websocket request
			websocketAccept(conn,buf,buflen);
			ESP_LOGD(TAG,"websocketAccept socket: %d",conn);
			return false;
		} else {
			c += 4;
			char* c_end = strstr(c, "HTTP");
			if(c_end == NULL) return true;
			*(c_end-1) = 0;
			c_end = strstr(c,"?");
//
// web command api,
///////////////////
			if(c_end != NULL) // commands api
			{
				char* param;
//printf("GET commands  socket:%d command:%s\n",conn,c);
// uart command
				param = strstr(c,"uart") ;
				if (param != NULL) {uart_set_baudrate(0, 115200);} //UART_SetBaudrate(0, 115200);}
// volume command
				param = getParameterFromResponse("volume=", c, strlen(c)) ;
				if ((param != NULL)&&(atoi(param)>=0)&&(atoi(param)<=254))
				{
					setVolume(param);
					wsVol(param);
				}
				infree(param);
// play command
				param = getParameterFromResponse("play=", c, strlen(c)) ;
				if (param != NULL) {playStation(param);infree(param);}
// start command
				param = strstr(c,"start") ;
				if (param != NULL) {playStationInt(getCurrentStation());}
// stop command
				param = strstr(c,"stop") ;
				if (param != NULL) {clientDisconnect("Web stop");}
// next command
				param = strstr(c,"next") ;
				if (param != NULL) {wsStationNext();}
// prev command
				param = strstr(c,"prev") ;
				if (param != NULL) {wsStationPrev();}
// instantplay command
				param = getParameterFromComment("instant=", c, strlen(c)) ;
				if (param != NULL) {
					clientDisconnect("Web Instant");
					pathParse(param);
					clientParsePlaylist(param);
					infree(param);
					clientSetName("Instant Play",255);
					clientConnectOnce();
					vTaskDelay(1);
				}
// version command
				param = strstr(c,"version") ;
				if (param != NULL) {
					char vr[30];// = malloc(30);
					sprintf(vr,"Release: %s, Revision: %s (%s)", RELEASE, REVISION, WS_SOCKET_VERSION);
					printf("Version:%s\n",vr);
					respOk(conn,vr);
					return true;
				}
// infos command
				param = strstr(c,"infos") ;
				if (param != NULL) {
					char* vr = webInfo();
					respOk(conn,vr);
					infree(vr);
					return true;
				}
// list command	 ?list=1 to list the name of the station 1
				param = getParameterFromResponse("list=", c, strlen(c)) ;
				if ((param != NULL)&&(atoi(param)>=0)&&(atoi(param)<=254))
				{
					char* vr = webList(atoi(param));
					respOk(conn,vr);
					infree(vr);
					return true;
				}
				respOk(conn,NULL); // response OK to the origin
			}
			else
// file GET
			{
				if(strlen(c) > 32) {
					respKo(conn);
					return true;}
				ESP_LOGV(TAG,"GET file  socket:%d file:%s",conn,c);
				serveFile(c, conn);
				ESP_LOGV(TAG,"GET end socket:%d file:%s",conn,c);
			}
		}
	} else if( (c = strstr(buf, "POST ")) != NULL) {
// a post request
		ESP_LOGV(TAG,"POST socket: %d  buflen: %d",conn,buflen);
		char fname[32];
		uint8_t i;
		for(i=0; i<32; i++) fname[i] = 0;
		c += 5;
		char* c_end = strstr(c, " ");
		if(c_end == NULL) return true;
		uint8_t len = c_end-c;
		if(len > 32) return true;
		strncpy(fname, c, len);
		ESP_LOGV(TAG,"POST Name: %s", fname);
		// DATA
		char* d_start = strstr(buf, "\r\n\r\n");
//		ESP_LOGV(TAG,"dstart:%s",d_start);
		if(d_start != NULL) {
			d_start += 4;
			uint16_t len = buflen - (d_start-buf);
			handlePOST(fname, d_start, len, conn);
		}
	}
	return true;
}


#define RECLEN	768
#define DRECLEN (RECLEN*2)
// Server child task to handle a request from a browser.
void serverclientTask(void *pvParams) {
	struct timeval timeout;
	timeout.tv_sec = 6;
	timeout.tv_usec = 0;
	int recbytes ,recb;
	char  buf[DRECLEN];
	portBASE_TYPE uxHighWaterMark;
	int  client_sock =  (int)pvParams;
 //   char *buf = (char *)inmalloc(reclen);
	bool result = true;

	if (buf != NULL)
	{
		memset(buf,0,DRECLEN);
		if (setsockopt (client_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
			printf(strsSOCKET,"setsockopt",errno);

		while (((recbytes = read(client_sock , buf, RECLEN)) != 0))
		{ // For now we assume max. RECLEN bytes for request
			if (recbytes < 0) {
				break;
				if (errno != EAGAIN )
				{
					printf(strsSOCKET,"client_sock",errno);
					vTaskDelay(10);
					break;
				} else {printf(strsSOCKET,tryagain,errno);break;}
			}
			char* bend = NULL;
			do {
				bend = strstr(buf, "\r\n\r\n");
				if (bend != NULL)
				{
					bend += 4;
					if (strstr(buf,"POST") ) //rest of post?
					{
						uint16_t cl = atoi(strstr(buf, "Content-Length: ")+16);
						vTaskDelay(1);
						if ((bend - buf +cl)> recbytes)
						{
//printf ("Server: try receive more:%d bytes. , must be %d\n", recbytes,bend - buf +cl);
							while(((recb = read(client_sock , buf+recbytes, cl))==0)||(errno == EAGAIN))
							{
								vTaskDelay(1);
								if ((recb < 0)&&(errno != EAGAIN)) {
									ESP_LOGE(TAG,"read fails 0  errno:%d",errno);
									respKo(client_sock);
									break;
								} else recb = 0;
							}
//							printf ("Server: received more for end now: %d bytes\n", recbytes+recb);
							buf[recbytes+recb] = 0;
							recbytes += recb;
						}
					}
				}
				else {

//					printf ("Server: try receive more for end:%d bytes\n", recbytes);
					while(((recb= read(client_sock , buf+recbytes, DRECLEN-recbytes))==0)||(errno == EAGAIN))
					{
						vTaskDelay(1);
//						printf ("Server: received more for end now: %d bytes\n", recbytes+recb);
						if ((recb < 0)&&(errno != EAGAIN)) {
							ESP_LOGE(TAG,"read fails 1  errno:%d",errno);
							respKo(client_sock);
							break;
						} else recb = 0;
					}
					recbytes += recb;
				} //until "\r\n\r\n"
			} while (bend == NULL);
			if (bend != NULL)
				result = httpServerHandleConnection(client_sock, buf, recbytes);
			memset(buf,0,DRECLEN);
			if (!result)
			{
				break; // only a websocket created. exit without closing the socket
			}
			vTaskDelay(1);
		}
//		infree(buf);
	} else  printf(strsMALLOC1,"buf");
	if (result)
	{
		int err;
		err = close(client_sock);
		if (err != ERR_OK)
		{
			err=close(client_sock);
//			printf ("closeERR:%d\n",err);
		}
	}
	xSemaphoreGive(semclient);
	ESP_LOGV(TAG,"Give client_sock: %d",client_sock);
	uxHighWaterMark = uxTaskGetStackHighWaterMark( NULL );
	ESP_LOGD(TAG,"watermark serverClientTask: %x  %d",uxHighWaterMark,uxHighWaterMark);

	vTaskDelete( NULL );
}
