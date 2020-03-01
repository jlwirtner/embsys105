/************************************************************************************

Copyright (c) 2001-2016  University of Washington Extension.

Module Name:

    tasks.c

Module Description:

    The tasks that are executed by the test application.

2016/2 Nick Strathy adapted it for NUCLEO-F401RE 

************************************************************************************/
#include <stdarg.h>

#include "bsp.h"
#include "print.h"
#include "mp3Util.h"

#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ILI9341.h>
#include <Adafruit_FT6206.h>

Adafruit_ILI9341 lcdCtrl = Adafruit_ILI9341(); // The LCD controller

Adafruit_FT6206 touchCtrl = Adafruit_FT6206(); // The touch controller

//BUTTONS

Adafruit_GFX_Button playButton = Adafruit_GFX_Button();
Adafruit_GFX_Button stopButton = Adafruit_GFX_Button();

#define PENRADIUS 3

long MapTouchToScreen(long x, long in_min, long in_max, long out_min, long out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}


#include "train_crossing.h"

#define BUFSIZE 256

/************************************************************************************

   Allocate the stacks for each task.
   The maximum number of tasks the application can have is defined by OS_MAX_TASKS in os_cfg.h

************************************************************************************/

static OS_STK   TouchTaskStk[APP_CFG_TASK_START_STK_SIZE];
static OS_STK   DisplayTaskStk[APP_CFG_TASK_START_STK_SIZE];
static OS_STK   Mp3DemoTaskStk[APP_CFG_TASK_START_STK_SIZE];
static OS_STK   CommandTaskStk[APP_CFG_TASK_START_STK_SIZE];
static OS_STK   Mp3TaskStk[APP_CFG_TASK_START_STK_SIZE];

     
// Task prototypes
void TouchTask(void* pdata);
void DisplayTask(void* pdata);
void Mp3DemoTask(void* pdata);
void CommandTask(void* pdata);
void Mp3Task(void* pdata);



// Useful functions
void PrintToLcdWithBuf(char *buf, int size, char *format, ...);


// Globals
BOOLEAN nextSong = OS_FALSE;

// Message queue nonsense

#define QUEUE_SIZE 4

typedef enum {
  play,
  stop
} commands;

OS_EVENT * commandMsgQ;

void* commandMsg[QUEUE_SIZE];

commands commandMem[QUEUE_SIZE];

// Mailboxes

typedef enum {
  init,
  pause,
  startPlayback,
  playback,
  stopPlayback
} mp3PlayerState;

typedef enum {
  startDisplay,
  playDisplay,
  pauseDisplay
} displayState;

OS_EVENT * mp3MBox;
OS_EVENT * displayMBox;

void updateMp3PlayerState(mp3PlayerState* state, commands currentCommand);
void notifyDisplayIfNeeded(mp3PlayerState* state, commands currentCommand);

/************************************************************************************

   This task is the initial task running, started by main(). It starts
   the system tick timer and creates all the other tasks. Then it deletes itself.

************************************************************************************/
void StartupTask(void* pdata)
{
	char buf[BUFSIZE];

    PjdfErrCode pjdfErr;
    INT32U length;
    static HANDLE hSD = 0;
    static HANDLE hSPI = 0;

	PrintWithBuf(buf, BUFSIZE, "StartupTask: Begin\n");
	PrintWithBuf(buf, BUFSIZE, "StartupTask: Starting timer tick\n");

    // Start the system tick
    OS_CPU_SysTickInit(OS_TICKS_PER_SEC);
    
    // Initialize SD card
    PrintWithBuf(buf, PRINTBUFMAX, "Opening handle to SD driver: %s\n", PJDF_DEVICE_ID_SD_ADAFRUIT);
    hSD = Open(PJDF_DEVICE_ID_SD_ADAFRUIT, 0);
    if (!PJDF_IS_VALID_HANDLE(hSD)) while(1);


    PrintWithBuf(buf, PRINTBUFMAX, "Opening SD SPI driver: %s\n", SD_SPI_DEVICE_ID);
    // We talk to the SD controller over a SPI interface therefore
    // open an instance of that SPI driver and pass the handle to 
    // the SD driver.
    hSPI = Open(SD_SPI_DEVICE_ID, 0);
    if (!PJDF_IS_VALID_HANDLE(hSPI)) while(1);
    
    length = sizeof(HANDLE);
    pjdfErr = Ioctl(hSD, PJDF_CTRL_SD_SET_SPI_HANDLE, &hSPI, &length);
    if(PJDF_IS_ERROR(pjdfErr)) while(1);

    // Create the test tasks
    PrintWithBuf(buf, BUFSIZE, "StartupTask: Creating the application tasks\n");
    
    commandMsgQ = OSQCreate(&commandMsg[0], QUEUE_SIZE);
    mp3MBox = OSMboxCreate((void*)0);
    displayMBox = OSMboxCreate((void*)0);

    // The maximum number of tasks the application can have is defined by OS_MAX_TASKS in os_cfg.h
    //OSTaskCreate(Mp3DemoTask, (void*)0, &Mp3DemoTaskStk[APP_CFG_TASK_START_STK_SIZE-1], APP_TASK_TEST1_PRIO);
    OSTaskCreate(TouchTask, (void*)0, &TouchTaskStk[APP_CFG_TASK_START_STK_SIZE-1], APP_TASK_TEST2_PRIO);
    OSTaskCreate(DisplayTask, (void*)0, &DisplayTaskStk[APP_CFG_TASK_START_STK_SIZE-1], 8);
    OSTaskCreate(CommandTask, (void*)0, &CommandTaskStk[APP_CFG_TASK_START_STK_SIZE-1], 3);
    OSTaskCreate(Mp3Task, (void*)0, &Mp3TaskStk[APP_CFG_TASK_START_STK_SIZE-1], 5);
    

    // Delete ourselves, letting the work be done in the new tasks.
    PrintWithBuf(buf, BUFSIZE, "StartupTask: deleting self\n");
	OSTaskDel(OS_PRIO_SELF);
}

void CommandTask(void* pdata)
{
    char buf[BUFSIZE];
	PrintWithBuf(buf, BUFSIZE, "CommandTask: starting\n");
    
    INT8U err;
    commands* pCurrentCommand;
    while(1) {
        pCurrentCommand = (commands*)OSQPend(commandMsgQ, 0, &err);
        PrintWithBuf(buf, BUFSIZE, "CommandTask: received command!\n");
        switch(*pCurrentCommand) {
        case play:
            PrintWithBuf(buf, BUFSIZE, "CommandTask: Pressed play!\n");
            err = OSMboxPost(mp3MBox, (void*)pCurrentCommand);
            if(err != 0) {
                PrintWithBuf(buf, BUFSIZE, "CommandTask: error posting to mp3 mailbox - %d!\n", err);
            }
            
            break;
        case stop:
            PrintWithBuf(buf, BUFSIZE, "CommandTask: Pressed stop!\n");
            err = OSMboxPost(mp3MBox, (void*)pCurrentCommand);
            if(err != 0) {
                PrintWithBuf(buf, BUFSIZE, "CommandTask: error posting to mp3 mailbox - %d!\n", err);
            }
            break;
        }
        OSTimeDly(5);
    }
}

void Mp3Task(void* pdata)
{
    PjdfErrCode pjdfErr;
    INT32U length;
    
    char buf[BUFSIZE];
    PrintWithBuf(buf, BUFSIZE, "Mp3Task: starting\n");
    
    PrintWithBuf(buf, BUFSIZE, "Opening MP3 driver: %s\n", PJDF_DEVICE_ID_MP3_VS1053);
    // Open handle to the MP3 decoder driver
    HANDLE hMp3 = Open(PJDF_DEVICE_ID_MP3_VS1053, 0);
    if (!PJDF_IS_VALID_HANDLE(hMp3)) while(1);

	PrintWithBuf(buf, BUFSIZE, "Opening MP3 SPI driver: %s\n", MP3_SPI_DEVICE_ID);
    // We talk to the MP3 decoder over a SPI interface therefore
    // open an instance of that SPI driver and pass the handle to 
    // the MP3 driver.
    HANDLE hSPI = Open(MP3_SPI_DEVICE_ID, 0);
    if (!PJDF_IS_VALID_HANDLE(hSPI)) while(1);

    length = sizeof(HANDLE);
    pjdfErr = Ioctl(hMp3, PJDF_CTRL_MP3_SET_SPI_HANDLE, &hSPI, &length);
    if(PJDF_IS_ERROR(pjdfErr)) while(1);

    // Send initialization data to the MP3 decoder and run a test
	PrintWithBuf(buf, BUFSIZE, "Starting MP3 device test\n");
    Mp3Init(hMp3);
    PrintWithBuf(buf, BUFSIZE, "Finished MP3 device test\n");
    OSTimeDly(500);
    
    commands* pCurrentCommand;
    
    mp3PlayerState state = init;
    displayState newDisplayState = startDisplay;

    INT32U bufLen = sizeof(Train_Crossing);
    
    // mp3 stream variables
    INT8U *bufPos = (INT8U*)Train_Crossing;
    INT32U iBufPos = 0;
    INT32U chunkLen;
    
    chunkLen = MP3_DECODER_BUF_SIZE;
    
    BOOLEAN notifyPause = false;
    
    while(1) {
        pCurrentCommand = (commands*)OSMboxAccept(mp3MBox);
        if(pCurrentCommand) {
            //notifyDisplayIfNeeded(&state, *pCurrentCommand);
            if(state == pause && *pCurrentCommand == play) {
                newDisplayState = playDisplay;
                OSMboxPost(displayMBox, (void*)&newDisplayState);
                notifyPause = true;
            }
            updateMp3PlayerState(&state, *pCurrentCommand);
        }
        switch(state) {
        case init:
        case pause:
            if(notifyPause) {
                newDisplayState = pauseDisplay;
                OSMboxPost(displayMBox, (void*)&newDisplayState);
                notifyPause = false;
            }
            OSTimeDly(50);
            break;
        case startPlayback:
            chunkLen = MP3_DECODER_BUF_SIZE;
            iBufPos = 0;
            bufPos = (INT8U*)Train_Crossing;
            // Place MP3 driver in command mode (subsequent writes will be sent to the decoder's command interface)
            Ioctl(hMp3, PJDF_CTRL_MP3_SELECT_COMMAND, 0, 0);
            
            // Reset the device
            length = BspMp3SoftResetLen;
            Write(hMp3, (void*)BspMp3SoftReset, &length);
         
            // Set volume
            length = BspMp3SetVol1010Len;
            Write(hMp3, (void*)BspMp3SetVol1010, &length);

            // To allow streaming data, set the decoder mode to Play Mode
            length = BspMp3PlayModeLen;
            Write(hMp3, (void*)BspMp3PlayMode, &length);
           
            // Set MP3 driver to data mode (subsequent writes will be sent to decoder's data interface)
            Ioctl(hMp3, PJDF_CTRL_MP3_SELECT_DATA, 0, 0);
            
            newDisplayState = playDisplay;
            OSMboxPost(displayMBox, (void*)&newDisplayState);
            
            notifyPause = true;
            state = playback;
            break;
        case playback:
//            OSTimeDly(500);
//            PrintWithBuf(buf, BUFSIZE, "Begin streaming sound file  count=%d\n", ++count);
//            Mp3Stream(hMp3, (INT8U*)&Train_Crossing[playbackMarker], chunkSize);
//            playbackMarker += chunkSize;
//            if (playbackMarker >= end) {
//                PrintWithBuf(buf, BUFSIZE, "Done streaming sound file  count=%d\n", count);
//                state = init;
//            }
            // detect last chunk of pBuf
            if (bufLen - iBufPos < MP3_DECODER_BUF_SIZE)
            {
                chunkLen = bufLen - iBufPos;
                state = stopPlayback;
            }
                    
            Write(hMp3, bufPos, &chunkLen);
                    
            bufPos += chunkLen;
            iBufPos += chunkLen;
            OSTimeDly(1);
            break;
        case stopPlayback:
            Ioctl(hMp3, PJDF_CTRL_MP3_SELECT_COMMAND, 0, 0);
            length = BspMp3SoftResetLen;
            Write(hMp3, (void*)BspMp3SoftReset, &length);
            newDisplayState = startDisplay;
            OSMboxPost(displayMBox, (void*)&newDisplayState);
            notifyPause = false;
            state = init;
            break;
        }
    }
}

void updateMp3PlayerState(mp3PlayerState* state, commands currentCommand)
{
  if(*state == init && currentCommand == play) {
    *state = startPlayback;
    return;
  }
  
  if((*state == playback || *state == pause) && currentCommand == stop) {
    *state = stopPlayback;
    return;
  }
  
  if(*state == playback && currentCommand == play) {
    *state = pause;
    return;
  }
  
  if(*state == pause && currentCommand == play) {
    *state = playback;
    return;
  }
}

void notifyDisplayIfNeeded(mp3PlayerState* state, commands currentCommand)
{
  INT8U err;
  displayState newDisplayState;
  
  char buf[BUFSIZE];
  
  if(*state == init && currentCommand == play) {
    newDisplayState = playDisplay;
    err = OSMboxPost(displayMBox,(void*)newDisplayState);
    if(err != OS_ERR_NONE) {
        PrintWithBuf(buf, BUFSIZE, "err posting to display mailbox! - %d\n", err);
    } else {
        PrintWithBuf(buf, BUFSIZE, "notified display\n");
    }
    return;
  }
  
  if(*state == stopPlayback) {
    newDisplayState = startDisplay;
    err = OSMboxPost(displayMBox,(void*)newDisplayState);
    if(err != OS_ERR_NONE) {
        PrintWithBuf(buf, BUFSIZE, "err posting to display mailbox! - %d\n", err);
    } else {
        PrintWithBuf(buf, BUFSIZE, "notified display\n");
    }
    return;
  }
  
  if(*state == playback && currentCommand == play) {
    newDisplayState = pauseDisplay;
    err = OSMboxPost(displayMBox,(void*)newDisplayState);
    if(err != OS_ERR_NONE) {
        PrintWithBuf(buf, BUFSIZE, "err posting to display mailbox! - %d\n", err);
    } else {
        PrintWithBuf(buf, BUFSIZE, "notified display\n");
    }
    return;
  }
  
  if(*state == pause && currentCommand == play) {
    newDisplayState = playDisplay;
    err = OSMboxPost(displayMBox,(void*)newDisplayState);
    if(err != OS_ERR_NONE) {
        PrintWithBuf(buf, BUFSIZE, "err posting to display mailbox! - %d\n", err);
    } else {
        PrintWithBuf(buf, BUFSIZE, "notified display\n");
    }
    return;
  }
}

static void DrawLcdContents()
{
    lcdCtrl.setRotation(180);
    playButton.initButton(&lcdCtrl, 75, 200, 75, 75, ILI9341_WHITE, ILI9341_BLUE, ILI9341_WHITE, "play", 2);
    stopButton.initButton(&lcdCtrl, 175, 200, 75, 75, ILI9341_WHITE, ILI9341_RED, ILI9341_WHITE, "stop", 2);
    
	char buf[BUFSIZE];
    lcdCtrl.fillScreen(ILI9341_BLACK);
    
    // Print a message on the LCD
    lcdCtrl.setCursor(40, 60);
    lcdCtrl.setTextColor(ILI9341_WHITE);  
    lcdCtrl.setTextSize(2);
    PrintToLcdWithBuf(buf, BUFSIZE, "Sup Brahhj!");
    playButton.drawButton();
    stopButton.drawButton();

}

void DrawPlayDisplay()
{
    char buf[BUFSIZE];
    lcdCtrl.fillRect(40, 80, 125, 20, ILI9341_BLACK);
    lcdCtrl.setCursor(40, 80);
    lcdCtrl.setTextColor(ILI9341_WHITE);  
    lcdCtrl.setTextSize(2);
    PrintToLcdWithBuf(buf, BUFSIZE, "playing...");
}

void DrawPauseDisplay()
{
    char buf[BUFSIZE];
    lcdCtrl.fillRect(40, 80, 125, 20, ILI9341_BLACK);
    lcdCtrl.setCursor(40, 80);
    lcdCtrl.setTextColor(ILI9341_WHITE);  
    lcdCtrl.setTextSize(2);
    PrintToLcdWithBuf(buf, BUFSIZE, "paused... ");
}

void DrawStartDisplay()
{
    char buf[BUFSIZE];
    lcdCtrl.fillRect(40, 80, 125, 20, ILI9341_BLACK);
//    lcdCtrl.setCursor(40, 80);
//    lcdCtrl.setTextColor(ILI9341_WHITE);  
//    lcdCtrl.setTextSize(2);
//    PrintToLcdWithBuf(buf, BUFSIZE, "          ");
}

void DisplayTask(void* pdata)
{
    PjdfErrCode pjdfErr;
    INT32U length;

	char buf[BUFSIZE];
	PrintWithBuf(buf, BUFSIZE, "UpdateDisplayTask: starting\n");

	PrintWithBuf(buf, BUFSIZE, "Opening LCD driver: %s\n", PJDF_DEVICE_ID_LCD_ILI9341);
    // Open handle to the LCD driver
    HANDLE hLcd = Open(PJDF_DEVICE_ID_LCD_ILI9341, 0);
    if (!PJDF_IS_VALID_HANDLE(hLcd)) while(1);

	PrintWithBuf(buf, BUFSIZE, "Opening LCD SPI driver: %s\n", LCD_SPI_DEVICE_ID);
    // We talk to the LCD controller over a SPI interface therefore
    // open an instance of that SPI driver and pass the handle to 
    // the LCD driver.
    HANDLE hSPI = Open(LCD_SPI_DEVICE_ID, 0);
    if (!PJDF_IS_VALID_HANDLE(hSPI)) while(1);

    length = sizeof(HANDLE);
    pjdfErr = Ioctl(hLcd, PJDF_CTRL_LCD_SET_SPI_HANDLE, &hSPI, &length);
    if(PJDF_IS_ERROR(pjdfErr)) while(1);

	PrintWithBuf(buf, BUFSIZE, "Initializing LCD controller\n");
    lcdCtrl.setPjdfHandle(hLcd);
    lcdCtrl.begin();

    DrawLcdContents();
    
    INT8U err;
    displayState* pNewDisplay;
    
    while(1) {
        pNewDisplay = (displayState*)OSMboxPend(displayMBox, 0, &err);
        PrintWithBuf(buf, BUFSIZE, "DisplayTask: unpended! - %d\n", err);
        switch(*pNewDisplay) {
        case startDisplay:
            PrintWithBuf(buf, BUFSIZE, "DisplayTask: update - start!\n");
            DrawStartDisplay();
            break;
        case playDisplay:
            PrintWithBuf(buf, BUFSIZE, "DisplayTask: update - play!\n");
            DrawPlayDisplay();
            break;
        case pauseDisplay:
            PrintWithBuf(buf, BUFSIZE, "DisplayTask: update - pause!\n");
            DrawPauseDisplay();
            break;
        }
        OSTimeDly(5);
    }
    
}



/************************************************************************************

   Runs Touch Task

************************************************************************************/
void TouchTask(void* pdata)
{
    PjdfErrCode pjdfErr;
    INT32U length;

	char buf[BUFSIZE];
	PrintWithBuf(buf, BUFSIZE, "LcdTouchDemoTask: starting\n");
    
    HANDLE hI2c1 = Open(PJDF_DEVICE_ID_I2C1, NULL);
    if (!PJDF_IS_VALID_HANDLE(hI2c1)) while(1);
    touchCtrl.setPjdfHandle(hI2c1);
    
    PrintWithBuf(buf, BUFSIZE, "Initializing FT6206 touchscreen controller\n");
    if (! touchCtrl.begin(40)) {  // pass in 'sensitivity' coefficient
        PrintWithBuf(buf, BUFSIZE, "Couldn't start FT6206 touchscreen controller\n");
        while (1);
    }
    
    int currentcolor = ILI9341_RED;
    
    commands currentCommand;
    INT8U err;

    while (1) { 
        boolean touched = false;
        
        // TODO: Poll for a touch on the touch panel
        // <Your code here>
        // <hint: Call a function provided by touchCtrl
        touched = touchCtrl.touched();
        if (! touched) {
            playButton.press(false);
            stopButton.press(false);
            OSTimeDly(5);
            continue;
        }
        
        TS_Point rawPoint;
       
        // TODO: Retrieve a point  
        // <Your code here>
        rawPoint = touchCtrl.getPoint();

        if (rawPoint.x == 0 && rawPoint.y == 0)
        {
            continue; // usually spurious, so ignore
        }
        
        if (playButton.contains(ILI9341_TFTWIDTH - rawPoint.x, ILI9341_TFTHEIGHT - rawPoint.y) && !playButton.isPressed()){
            playButton.press(true);
            currentCommand = play;
            err = OSQPost(commandMsgQ, (void*)&currentCommand);
            if (err != 0) {
                PrintWithBuf(buf, BUFSIZE, "error!\n");
            } else {
                OSTimeDly(5);
            }
            continue; // only draw inside of playbutton
        }
        
        if (stopButton.contains(ILI9341_TFTWIDTH - rawPoint.x, ILI9341_TFTHEIGHT - rawPoint.y) && !stopButton.isPressed()){
            stopButton.press(true);
            currentCommand = stop;
            err = OSQPost(commandMsgQ, (void*)&currentCommand);
            if (err != 0) {
                PrintWithBuf(buf, BUFSIZE, "error!\n");
            } else {
                OSTimeDly(5);
            }
            continue; // only draw inside of playbutton
        }
    }
}
/************************************************************************************

   Runs MP3 demo code

************************************************************************************/
void Mp3DemoTask(void* pdata)
{
    PjdfErrCode pjdfErr;
    INT32U length;

    OSTimeDly(2000); // Allow other task to initialize LCD before we use it.
    
	char buf[BUFSIZE];
	PrintWithBuf(buf, BUFSIZE, "Mp3DemoTask: starting\n");

	PrintWithBuf(buf, BUFSIZE, "Opening MP3 driver: %s\n", PJDF_DEVICE_ID_MP3_VS1053);
    // Open handle to the MP3 decoder driver
    HANDLE hMp3 = Open(PJDF_DEVICE_ID_MP3_VS1053, 0);
    if (!PJDF_IS_VALID_HANDLE(hMp3)) while(1);

	PrintWithBuf(buf, BUFSIZE, "Opening MP3 SPI driver: %s\n", MP3_SPI_DEVICE_ID);
    // We talk to the MP3 decoder over a SPI interface therefore
    // open an instance of that SPI driver and pass the handle to 
    // the MP3 driver.
    HANDLE hSPI = Open(MP3_SPI_DEVICE_ID, 0);
    if (!PJDF_IS_VALID_HANDLE(hSPI)) while(1);

    length = sizeof(HANDLE);
    pjdfErr = Ioctl(hMp3, PJDF_CTRL_MP3_SET_SPI_HANDLE, &hSPI, &length);
    if(PJDF_IS_ERROR(pjdfErr)) while(1);

    // Send initialization data to the MP3 decoder and run a test
	PrintWithBuf(buf, BUFSIZE, "Starting MP3 device test\n");
    Mp3Init(hMp3);
    int count = 0;
    
    while (1)
    {
        OSTimeDly(500);
        PrintWithBuf(buf, BUFSIZE, "Begin streaming sound file  count=%d\n", ++count);
        Mp3Stream(hMp3, (INT8U*)Train_Crossing, sizeof(Train_Crossing)); 
        PrintWithBuf(buf, BUFSIZE, "Done streaming sound file  count=%d\n", count);
    }
}


// Renders a character at the current cursor position on the LCD
static void PrintCharToLcd(char c)
{
    lcdCtrl.write(c);
}

/************************************************************************************

   Print a formated string with the given buffer to LCD.
   Each task should use its own buffer to prevent data corruption.

************************************************************************************/
void PrintToLcdWithBuf(char *buf, int size, char *format, ...)
{
    va_list args;
    va_start(args, format);
    PrintToDeviceWithBuf(PrintCharToLcd, buf, size, format, args);
    va_end(args);
}




