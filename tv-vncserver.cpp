/*
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/sysmacros.h>             /* For makedev() */

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include <assert.h>
#include <errno.h>

#include <binder/ProcessState.h>

#include <binder/IServiceManager.h>
#include <binder/IMemory.h>

#include <gui/BufferQueue.h>
#include <gui/CpuConsumer.h>
#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>

#include <ui/DisplayInfo.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>

#include <android/hardware/tv/cec/1.0/IHdmiCec.h>

/* libvncserver */
#include "rfb/rfb.h"
#include "rfb/keysym.h"

using namespace android;
using namespace android::hardware::tv::cec::V1_0;

struct Info {
    size_t      size;
    size_t      bitsPerPixel;
    struct {
        uint8_t     al;
        uint8_t     as;
        uint8_t     redLength;
        uint8_t     redShift;
        uint8_t     greenLength;
        uint8_t     greenShift;
        uint8_t     blueLength;
        uint8_t     blueShift;
    };
    PixelFormat	format;
};

typedef struct _XKEY_TO_KEY {
	int xkey;
	int vkey;
} XKEY_TO_KEY;

static const XKEY_TO_KEY keymap[] = {
	{ ' '	, KEY_SPACE },
	{ '0'	, KEY_0 },
	{ '1'	, KEY_1 },
	{ '2'	, KEY_2 },
	{ '3'	, KEY_3 },
	{ '4'	, KEY_4 },
	{ '5'	, KEY_5 },
	{ '6'	, KEY_6 },
	{ '7'	, KEY_7 },
	{ '8'	, KEY_8 },
	{ '9'	, KEY_9 },
	{ 'A'	, KEY_A },
	{ 'B'	, KEY_B },
	{ 'C'	, KEY_C },
	{ 'D'	, KEY_D },
	{ 'E'	, KEY_E },
	{ 'F'	, KEY_F },
	{ 'G'	, KEY_G },
	{ 'H'	, KEY_H },
	{ 'I'	, KEY_I },
	{ 'J'	, KEY_J },
	{ 'K'	, KEY_K },
	{ 'L'	, KEY_L },
	{ 'M'	, KEY_M },
	{ 'N'	, KEY_N },
	{ 'O'	, KEY_O },
	{ 'P'	, KEY_P },
	{ 'Q'	, KEY_Q },
	{ 'R'	, KEY_R },
	{ 'S'	, KEY_S },
	{ 'T'	, KEY_T },
	{ 'U'	, KEY_U },
	{ 'V'	, KEY_V },
	{ 'W', KEY_W },
	{ 'X'	, KEY_X },
	{ 'Y'	, KEY_Y },
	{ 'Z'	, KEY_Z },
	{ XK_Up		, KEY_UP },
	{ XK_Down	, KEY_DOWN },
	{ XK_Left	, KEY_LEFT },
	{ XK_Right	, KEY_RIGHT },
	{ XK_Page_Up	, KEY_PAGEUP},
	{ XK_Page_Down	, KEY_PAGEDOWN },
	{ XK_Return	, KEY_ENTER },
	{ XK_Escape	, KEY_ESC},
	{ XK_Menu	, KEY_MENU },
	{ XK_BackSpace, KEY_BACKSPACE },
	{ XK_Shift_L, KEY_LEFTSHIFT },
	{ XK_Shift_R, KEY_RIGHTSHIFT },
	{ XK_Pointer_Button3, KEY_BACK },
	{ 0, KEY_POWER },
	{ 0, KEY_WAKEUP },
};

class CVNC;

CVNC *vnc;

class CVNC
{
public:
	CVNC()
	{
		mVncPwd[0] = 0;
		mClients = 0;
		refresh = 150000;
		mVncPort = 5901;
		uifd = -1;
		line_skip_level = 2;
		key_shift_state	= 0;
		mVncPwdList[0] = mVncPwd;
		mVncPwdList[1] = 0;
	}

private:
	size_t 		fbsize;
	size_t		fblinesize;
	void		*fbbuf;
	suseconds_t	refresh;
	int			mVncPort;
	char*		mVncPwdList[2];
	char		mVncPwd[256] ;
	rfbScreenInfoPtr screen;

	int uifd;
	char hostname[128];

	int xres, yres, bpp;

	int line_skip_level;
	int line_skip_mask;
	int line_skip_offset;
	int line_single_offset;
	int line_skip_counter;

	int key_shift_state;

	int mClients;

	sp<IBinder> mVirtualDisplay;
	sp<IGraphicBufferProducer> mBufferProducer;
	sp<IGraphicBufferConsumer> mBufferConsumer;
	sp<CpuConsumer> mConsumer;
	ScreenshotClient *mScreenshotClient;
	sp<GraphicBuffer> mBuffer;
	sp<IBinder> mDisplay;

	static int translate_key(int xkey)
	{
		const XKEY_TO_KEY *k, *end;
		
		if (xkey >= 'a' && xkey <= 'z')
			xkey -= 'a' - 'A';

		end = sizeof(keymap) / sizeof(keymap[0]) + keymap;
		for (k = keymap; k < end; k++)
			if (k->xkey == xkey)
				return k->vkey;

		return 0;
	}

	void create_uinput(const char* dev_name, int xres, int yres)
	{
		struct uinput_user_dev uud;
		const XKEY_TO_KEY *k, *end;

		uifd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		ioctl(uifd, UI_SET_EVBIT, EV_KEY);
		ioctl(uifd, UI_SET_EVBIT, EV_SYN);
		ioctl(uifd, UI_SET_EVBIT, EV_ABS);

		ioctl(uifd, UI_SET_ABSBIT, ABS_X) ;
		ioctl(uifd, UI_SET_ABSBIT, ABS_Y);
		
		ioctl(uifd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

		end = sizeof(keymap) / sizeof(keymap[0]) + keymap;
		for (k = keymap; k < end; k++)
			ioctl(uifd, UI_SET_KEYBIT, k->vkey);

		ioctl(uifd, UI_SET_KEYBIT, BTN_TOUCH);

		
		memset(&uud, 0, sizeof(uud));
		strncpy(uud.name, dev_name, UINPUT_MAX_NAME_SIZE);
		uud.absmax[ABS_X] = xres - 1;
		uud.absmax[ABS_Y] = yres - 1;

		write(uifd, &uud, sizeof(uud));
		ioctl(uifd, UI_DEV_CREATE);
	}

	int uinput_write(int uinput_fd, uint16_t type, uint16_t code, int32_t value)
	{
		struct input_event event;

		event.time.tv_sec = 0;
		event.time.tv_usec = 0;
		event.type = type;
		event.code = code;
		event.value = value;
		return write(uinput_fd, &event, sizeof(event)) != sizeof(event) ? -1 : 0;
	}

	void onKey(rfbBool down, rfbKeySym xkey, rfbClientPtr cl)
	{
		int	vkey;

		if (uifd == -1 )
			return;

		if (xkey == XK_Shift_L || xkey == XK_Shift_R)
			key_shift_state = down;

		vkey = translate_key(xkey);
		if (!vkey)
			return;

		uinput_write(uifd, EV_KEY, vkey, down ? 1 : 0);
		uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
	}


	#define PTR_LEFT_BUTTON		1<<0
	#define PTR_MIDDLE_BUTTON	1<<1
	#define PTR_RIGHT_BUTTON	1<<2

	void onPtr(int button_mask, int x, int y, rfbClientPtr cl)
	{
		static int last_mask = 0;
		int delta_mask = button_mask ^ last_mask;

		if (uifd == -1 )
			return;

		if (delta_mask & PTR_LEFT_BUTTON) {
			uinput_write(uifd, EV_KEY, BTN_TOUCH, (button_mask & PTR_LEFT_BUTTON) ? 1 : 0);
			uinput_write(uifd, EV_ABS, ABS_X, x);
			uinput_write(uifd, EV_ABS, ABS_Y, y);
			uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
		} else if (button_mask & PTR_LEFT_BUTTON) {
			uinput_write(uifd, EV_ABS, ABS_X, x);
			uinput_write(uifd, EV_ABS, ABS_Y, y);
			uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
		}
		
		if (delta_mask & PTR_MIDDLE_BUTTON) {
			uinput_write(uifd, EV_ABS, ABS_X, x);
			uinput_write(uifd, EV_ABS, ABS_Y, y);
			uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
			uinput_write(uifd, EV_KEY, KEY_MENU, (button_mask & PTR_MIDDLE_BUTTON) ? 1 : 0);
			uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
		}

		if (delta_mask & PTR_RIGHT_BUTTON) {
			uinput_write(uifd, EV_KEY, KEY_BACK, (button_mask & PTR_RIGHT_BUTTON) ? 1 : 0);
			uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
		}
		
		last_mask = button_mask;
	}

	Info const *findFormat(PixelFormat pf)
	{
		static Info const sPixelFormatInfos[] = {
				{ 4, 32, { 8, 24,  8,  0,  8, 8,  8, 16 }, PIXEL_FORMAT_RGBA_8888  },
				{ 4, 24, { 0,  0,  8,  0,  8, 8,  8, 16 }, PIXEL_FORMAT_RGBX_8888 },
				{ 3, 24, { 0,  0,  8,  0,  8, 8,  8, 16 }, PIXEL_FORMAT_RGB_888 },
				{ 2, 16, { 0,  0,  5, 11,  6, 5,  5,  0 }, PIXEL_FORMAT_RGB_565 },
				{ 4, 32, { 8, 24,  8, 16,  8, 8,  8,  0 }, PIXEL_FORMAT_BGRA_8888},
				{ 2, 16, { 1,  0,  5, 11,  5, 6,  5,  1 }, PIXEL_FORMAT_RGBA_5551 },
				{ 2, 16, { 4,  0,  4, 12,  4, 8,  4,  4 }, PIXEL_FORMAT_RGBA_4444 },
		};

		for (const Info *pfi = sPixelFormatInfos; pfi < sPixelFormatInfos+ (sizeof(sPixelFormatInfos)/sizeof(sPixelFormatInfos[0])); pfi++)
			if (pfi->format == pf)
				return pfi;

		return 0;
	}

	void onClientGone(rfbClientPtr cl)
	{
		if (--mClients == 0) {
			sp<IHdmiCec>cec = IHdmiCec::getService();
			cec->getPhysicalAddress([this](Result result, uint16_t addr) {
				/* Power if last client and not connected */
				if (addr == 0xffff) {
					uinput_write(uifd, EV_KEY, KEY_POWER, 1);
					uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
					uinput_write(uifd, EV_KEY, KEY_POWER, 0);
					uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
				}
			});
		}
	}
	
	enum rfbNewClientAction onClient(rfbClientPtr cl)
	{
		if (mClients++ == 0) {
			sp<IHdmiCec>cec = IHdmiCec::getService();
			cec->getPhysicalAddress([this](Result result, uint16_t addr) {
				/* Wake up if first client and not connected */
				if (addr == 0xffff) {
					uinput_write(uifd, EV_KEY, KEY_WAKEUP, 1);
					uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
					uinput_write(uifd, EV_KEY, KEY_WAKEUP, 0);
					uinput_write(uifd, EV_SYN, SYN_REPORT, 0);
				}
			});
		}

		cl->clientGoneHook = onClientGoneWrapper;
		return RFB_CLIENT_ACCEPT;
	}

	int init_fb_server(int argc, char **argv)
	{
		status_t ret;

		mScreenshotClient = new ScreenshotClient();
		if (!mScreenshotClient) {
			fprintf(stderr, "%s: Could not create ScreenshotClient\n", __func__);
			return -1;
		}

		mDisplay = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);	

		Vector<DisplayInfo> configs;
		ret = SurfaceComposerClient::getDisplayConfigs(mDisplay, &configs);
		if (ret != NO_ERROR) {
			fprintf(stderr, "%s ERROR: unable to get display configs\n", __func__);
			return -1;
		}

		int activeConfig = SurfaceComposerClient::getActiveConfig(mDisplay);
		if(static_cast<size_t>(activeConfig) >= configs.size()) {
			fprintf(stderr, "%s Active config %d not inside configs (size %zu)\n", __func__, activeConfig, configs.size());
			return -1;
		}

		DisplayInfo dinfo = configs[activeConfig];
		xres = dinfo.w;
		yres = dinfo.h;

		const Info	*format = findFormat(PIXEL_FORMAT_RGBA_8888);

		mBuffer =  new GraphicBuffer(xres, yres, format->format, GraphicBuffer::USAGE_SW_READ_OFTEN | GraphicBuffer::USAGE_HW_TEXTURE);
		mScreenshotClient->capture(mDisplay, Rect(), 0, 0, 0, 0, true, ISurfaceComposer::eRotateNone, &mBuffer);
                                   
		if (ret != NO_ERROR) {
			fprintf(stderr, "%s ERROR: unable to get screenshot\n", __func__);
			return -1;
		}

		bpp = format->bitsPerPixel;
		fbsize = xres * yres * bpp>>3;
		fbbuf = calloc(fbsize, 1);
		if (!fbbuf) {
			fprintf(stderr, "%s: cannot allocate memory for buffer\n", __func__);
			return -ENOMEM;
		}

		const void *buffer;

		mBuffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, (void**)&buffer);
		memcpy(fbbuf, buffer, fbsize);
		mBuffer->unlock();
		
		screen = rfbGetScreen(&argc, argv, xres, yres, format->redLength, 3, bpp>>3);
		if (!screen) {
			fprintf(stderr, "%s: rfbGetScreen failed\n", __func__);
			return -ENOMEM;
		}

		rfbPixelFormat *pfmt = &(screen->serverFormat);
		pfmt->redShift = format->redShift;
		pfmt->greenShift = format->greenShift;
		pfmt->blueShift = format->blueShift;
		pfmt->redMax = (1 << format->redLength) -1;
		pfmt->greenMax = (1 << format->greenLength) -1;
		pfmt->blueMax = (1 << format->blueLength) -1;
		
		gethostname(hostname, sizeof(hostname));
		screen->desktopName = hostname;
		screen->frameBuffer = (char*)fbbuf;
		screen->alwaysShared = TRUE;
		screen->httpDir = NULL;
		screen->port = mVncPort;
		screen->ipv6port = mVncPort;
		if (mVncPwd[0]) {
			screen->authPasswdData = mVncPwdList;
			screen->passwordCheck = rfbCheckPasswordByList;
		}

		screen->kbdAddEvent = onKeyWrapper;
		screen->ptrAddEvent = onPtrWrapper;
		screen->newClientHook = onClientWrapper;

		rfbInitServer(screen);
		rfbMarkRectAsModified(screen, 0, 0, xres, yres);

		line_skip_counter = 0;
		line_skip_mask = (1 << line_skip_level) - 1;
		line_single_offset = (fbsize / yres) >> 2;
		line_skip_offset = line_single_offset * line_skip_mask;

		return 0;
	}

	void updateScreen()
	{
		int	min_x = 9999, max_x = -1;
		int	min_y = 9999, max_y = -1;
		int	change_detected;	
		const void*	buffer;
		status_t	ret;

		ret = mScreenshotClient->capture(mDisplay, Rect(), 0, 0, 0, 0, true, ISurfaceComposer::eRotateNone, &mBuffer);
		if (ret != NO_ERROR) {
			fprintf(stderr, "%s ERROR: unable to get screenshot\n", __func__);
			return;
		}

		mBuffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, (void**)&buffer);

		uint32_t *f = (uint32_t*)(fbsize + (uint8_t*)buffer);
		uint32_t *c = (uint32_t*)(fbsize + (uint8_t*)fbbuf);

		int x, y, y_prev;
		uint32_t pixel;

		for (y = y_prev = (int)yres; y--;) {
			change_detected = 0;
			
			if (y == min_y) {
				f -= fblinesize;
				c -= fblinesize;
				if (y)
					--y;
				continue;
			}
			
			/* Compare every 1/2/4 pixels at a time */
			for (x = (int)xres; x--; ) {
				pixel = *(--f);

				if (pixel != *(--c)) {
						*c = pixel;
						change_detected = 1;

						if (max_x < x)
							max_x = x;
						if (min_x > x)
							min_x = x;
					}
			}

			if (change_detected) {
				if (max_y < y)
					max_y = y;
				if (min_y > y)
					min_y = y;

				if (y_prev - 1 != y) {
					f += line_skip_offset;
					c += line_skip_offset;
					y += line_skip_mask;
				}
				y_prev = y;
			} else if ((y & line_skip_mask) == (line_skip_counter & line_skip_mask)) {
				y_prev = y;

				f -= line_skip_offset;
				c -= line_skip_offset;
				y -= line_skip_mask;
				if (y < 0)
					y = 0;
			} else
				y_prev = y;
		}

		if (min_x < 9999) {
//			fprintf(stderr, "Dirty page: %dx%d+%d+%d...\n",
//				(max_x+2) - min_x, (max_y+1) - min_y,
//				min_x, min_y);

			rfbMarkRectAsModified(screen, min_x, min_y,
				max_x + 2, max_y + 1);

			rfbProcessEvents(screen, 10000);
		}
		--line_skip_counter;

		mBuffer->unlock();
	}

	int printUsage(char *cmd)
	{
		fprintf(stderr, "%s [-f device] [-p port] [-h]\n"
					"-P port: VNC port, default is %d\n"
					"-p password\n"
					"-h : print this help\n",
					cmd, mVncPort);

		return -1;
	}

public:
	static enum rfbNewClientAction onClientWrapper(rfbClientPtr cl)
	{
		return vnc->onClient(cl);
	} 

	static void onClientGoneWrapper(rfbClientPtr cl)
	{
		vnc->onClientGone(cl);
	}

	static void onKeyWrapper(rfbBool down, rfbKeySym xkey, rfbClientPtr cl)
	{
		vnc->onKey(down, xkey, cl);
	}

	static void onPtrWrapper(int button_mask, int x, int y, rfbClientPtr cl)
	{
		vnc->onPtr(button_mask, x, y, cl);
	}

	int server(int argc, char **argv)
	{
		int				rc;

		for (int i = 1; i < argc; i++)
			if(*argv[i] == '-') {
				switch(*(argv[i] + 1)) {
				case 'P':
					i++;
					mVncPort = atoi(argv[i]);
					break;
				case 'p':
					i++;
					strncpy(mVncPwd, argv[i], sizeof(mVncPwd));
					break;
				case 'h':
				default:
					return printUsage(*argv);
				}
			} else
				return printUsage(*argv);

		ProcessState::self()->startThreadPool();

		rc = init_fb_server(argc, argv);
		if (rc)
			return rc;

		create_uinput("vnc", xres, yres);

		struct timeval now={0,0}, next={0,0};
		suseconds_t	remaining_time;

		while (1) {
			while (screen->clientHead == NULL)
				rfbProcessEvents(screen, 100000);

			gettimeofday(&now, NULL);
			if (now.tv_sec > next.tv_sec ||
				(now.tv_sec == next.tv_sec && now.tv_usec > next.tv_usec)) {

				next.tv_sec  = now.tv_sec;
				next.tv_usec = now.tv_usec + refresh;
				if (next.tv_usec > 1000000) {
					next.tv_usec -= 1000000;
					next.tv_sec++;
				}

				updateScreen();
				gettimeofday(&now, NULL);
			}

			remaining_time  = next.tv_sec - now.tv_sec;
			remaining_time *= 1000000;
			remaining_time += next.tv_usec;
			remaining_time -= now.tv_usec;

			rfbProcessEvents(screen, remaining_time);
		}
	}
	
	void onShutdown()
	{
		ioctl(uifd, UI_DEV_DESTROY);
		close(uifd);
	}
};

void on_bus_error(int signal)
{
	fprintf(stderr, "Bus error occurred. Try another grabber method\n");
	exit(1);
}

void on_shutdown(int signal)
{
	fprintf(stderr, "Cleaning up...\n");
	vnc->onShutdown();
	exit(0);
}

int main(int argc, char **argv)
{
	vnc = new CVNC();
	
	struct sigaction	sa;

	/* Install handler for graceful shutdown */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_shutdown;
	sigaction(SIGTERM, &sa, NULL) ;
	sigaction(SIGINT, &sa, NULL) ;

	sa.sa_handler = on_bus_error;
	sigaction(SIGBUS, &sa, NULL) ;

	return vnc->server(argc, argv);
}
