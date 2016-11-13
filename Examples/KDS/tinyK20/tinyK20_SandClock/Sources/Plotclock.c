// Plotclock
// cc - by Johannes Heberlein 2014
// v 1.01
// thingiverse.com/joo   wiki.fablab-nuernberg.de

// units: mm; microseconds; radians
// origin: bottom left of drawing surface

#include "Plotclock.h"
#include <math.h>
#include "Servo.h"
#include "FRTOS1.h"
#include "WAIT1.h"
#include "PCA9685.h"
#include "TmDt1.h"

static bool PlotClockIsEnabled = FALSE; /* if clock is enabled */
static bool PlotClockCalibrateIsOn = FALSE; /* if calibration mode is on */

/* When in calibration mode, adjust the following factor until the servos move exactly 90 degrees */
static int SERVOFAKTOR_LEFT = 700;
static int SERVOFAKTOR_RIGHT = 750;

/* Zero-position of left and right servo.
   When in calibration mode, adjust the NULL-values so that the servo arms are at all times parallel
   either to the X or Y axis */
static int SERVOLEFTNULL = 1525;
static int SERVORIGHTNULL = 425;

#define PLOTCLOCK_MINPOS_X   -30
#define PLOTCLOCK_MAXPOS_X   90

#define PLOTCLOCK_MINPOS_Y   10
#define PLOTCLOCK_MAXPOS_Y   75

#define PLOTCLOCK_PARKPOS_X  90
#define PLOTCLOCK_PARKPOS_Y  35

/* lift positions of lifting servo */
static int LIFT_DRAW = 1275; /* on drawing surface */
static int LIFT_BETWEEN_DRAW =  900;  /* between numbers */
static int LIFT_PARKING =  1000;  /* Parking position */
static int LIFT_INIT = 1000; /* start/reset position */
static int servoLift; /* current position of lift servo */

/* position of left lower corner for drawing */
static int16_t leftLowerCornerX = -5;
static int16_t leftLowerCornerY = 26;

/* corresponds to PCA9685 channels */
typedef enum {
  PLOTCLOCK_SERVO_LIFT = 0,
  PLOTCLOCK_SERVO_LEFT = 1,
  PLOTCLOCK_SERVO_RIGHT= 2,
  PLOTCLOCK_LED0 = 13,
  PLOTCLOCK_VIBRA1 = 14,
  PLOTCLOCK_VIBRA2 = 15,
} PlotClockServo;

typedef enum {
  PLOTCLOCK_LIFT_POS_DRAW,
  PLOTCLOCK_LIFT_POS_BETWEEN_DRAW,
  PLOTCLOCK_LIFT_POS_PARKING
} PlotClockLiftPos;

/* last values */
static volatile double lastX = PLOTCLOCK_PARKPOS_X;
static volatile double lastY = PLOTCLOCK_PARKPOS_Y;
static uint8_t last_min = -1;
static bool isWriting = FALSE;

/* speed (actually delay time) of lifting arm, higher is slower */
#define LIFTSPEED 1500

#if 0
/* length of arms in mm, see http://wiki.fablab-nuernberg.de/w/Datei:erklaerung.jpg */
#define L1 40.0 /* length of first arm, line from servo joint to the first joint */
#define L2 59.0 /* line from first joint to center of pen */
#define L3 13.0 /* length of 'front' arm, line from front joint to center of the pen */
#define L4 50.0 /* length of the second arm, distance between first joint and second joint */
#else
static float L1 = 40.0;
static float L2 = 59.0;
static float L3 = 13.0;
static float L4 = 50.0;
#endif
/* origin points in mm of left and right servo */
#if 0
#define O1X  22  /* X position of the left servo to left border.  */
#define O1Y -30  /* Y position of of left servo to base line. */
#define O2X  48  /* X position of the right servo */
#define O2Y -30  /* Y position of the right servo */
#else
static float O1X = 22.0;
static float O1Y = -30.0;
static float O2X = 48.0;
static float O2Y = -30.0;
#endif

#define M_PI    3.14159265358979323846

static void delay(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

static void delayMicroseconds(uint32_t us) {
  WAIT1_Waitus(us);
}

static void servo_writeMicroseconds(PlotClockServo servo, uint16_t us) {
  SERVO_WriteDutyMicroSeconds(PCA9685_I2C_DEFAULT_ADDR, (uint8_t)servo, us, SERVO_MIN_TICKS, SERVO_MAX_TICKS);
}

static double return_angle(double a, double b, double c) {
  /* cosine rule for angle between c and a */
  /* input to acos in range of -1 to +1 */
  double val = (a*a + c*c - b*b) / (2 * a * c);

  if (val>1.0) {
    val = 1.0;
  } else if (val<-1.0) {
    val = -1.0;
  }
  return acos(val);
}

static void set_XY(double Tx, double Ty) {
  double dx, dy, c, a1, a2, Hx, Hy;

  delay(1);
  /* calculate triangle between pen, servoLeft and arm joint */
  /* cartesian dx/dy */
  dx = Tx - O1X;
  dy = Ty - O1Y;

  /* polar length (c) and angle (a1) */
  c = sqrt(dx*dx + dy*dy);
  a1 = atan2(dy, dx);
  a2 = return_angle(L1, L2, c);

  servo_writeMicroseconds(PLOTCLOCK_SERVO_LEFT, floor(((a2 + a1 - M_PI) * SERVOFAKTOR_LEFT) + SERVOLEFTNULL));

  /* calculate join arm point for triangle of the right servo arm */
  a2 = return_angle(L2, L1, c);
  Hx = Tx + L3 * cos((a1 - a2 + 0.621) + M_PI); //36.5� is angle between marker and joint
  Hy = Ty + L3 * sin((a1 - a2 + 0.621) + M_PI);

  /* calculate triangle between pen joint, servoRight and arm joint */
  dx = Hx - O2X;
  dy = Hy - O2Y;

  c = sqrt(dx*dx + dy*dy);
  a1 = atan2(dy, dx);
  a2 = return_angle(L1, L4, c);

  servo_writeMicroseconds(PLOTCLOCK_SERVO_RIGHT, floor(((a1 - a2) * SERVOFAKTOR_RIGHT) + SERVORIGHTNULL));
}

static void drawTo(double pX, double pY) {
  double dx, dy, c;
  int i;

  if (pX<PLOTCLOCK_MINPOS_X) {
    pX = PLOTCLOCK_MINPOS_X;
  } else if (pX>PLOTCLOCK_MAXPOS_X) {
    pX = PLOTCLOCK_MAXPOS_X;
  }

  if (pY<PLOTCLOCK_MINPOS_Y) {
    pY = PLOTCLOCK_MINPOS_Y;
  } else if (pY>PLOTCLOCK_MAXPOS_Y) {
    pY = PLOTCLOCK_MAXPOS_Y;
  }

  /* dx dy of new point */
  dx = pX - lastX;
  dy = pY - lastY;
  //path length in mm, times 4 equals 4 steps per mm
#if 0 /* orig */
  c = floor(4 * sqrt(dx * dx + dy * dy));
#else
  c = floor(7 * sqrt(dx*dx + dy*dy)); /* c is the number of steps */
#endif

  if (c < 1) {
    c = 1;
  }
  for (i = 0; i <= c; i++) {
    /* draw line point by point */
    set_XY(lastX + (i * dx / c), lastY + (i * dy / c));
  }
  lastX = pX;
  lastY = pY;
}

static void bogenGZS(float bx, float by, float radius, int start, int ende, float sqee) {
  float inkr = 0.05;
  float count = 0;

  do {
    drawTo(sqee * radius * cos(start + count) + bx,
    radius * sin(start + count) + by);
    count += inkr;
  }
  while ((start + count) <= ende);
}

static void bogenUZS(float bx, float by, float radius, int start, int ende, float sqee) {
  float inkr = -0.05;
  float count = 0;

  do {
    drawTo(sqee * radius * cos(start + count) + bx,
    radius * sin(start + count) + by);
    count += inkr;
  }
  while ((start + count) > ende);
}

static void lift(PlotClockLiftPos lift) {
  switch (lift) {
    case PLOTCLOCK_LIFT_POS_DRAW:
      if (servoLift >= LIFT_DRAW) {
        while (servoLift >= LIFT_DRAW) {
          servoLift--;
          servo_writeMicroseconds(PLOTCLOCK_SERVO_LIFT, servoLift);
          delayMicroseconds(LIFTSPEED);
        }
      } else {
        while (servoLift <= LIFT_DRAW) {
          servoLift++;
          servo_writeMicroseconds(PLOTCLOCK_SERVO_LIFT, servoLift);
          delayMicroseconds(LIFTSPEED);
        }
      }
      break;

    case PLOTCLOCK_LIFT_POS_BETWEEN_DRAW:
      if (servoLift >= LIFT_BETWEEN_DRAW) {
        while (servoLift >= LIFT_BETWEEN_DRAW) {
          servoLift--;
          servo_writeMicroseconds(PLOTCLOCK_SERVO_LIFT, servoLift);
          delayMicroseconds(LIFTSPEED);
        }
      } else {
        while (servoLift <= LIFT_BETWEEN_DRAW) {
          servoLift++;
          servo_writeMicroseconds(PLOTCLOCK_SERVO_LIFT, servoLift);
          delayMicroseconds(LIFTSPEED);
        }
      }
      break;

    case PLOTCLOCK_LIFT_POS_PARKING:
      if (servoLift >= LIFT_PARKING) {
        while (servoLift >= LIFT_PARKING) {
          servoLift--;
          servo_writeMicroseconds(PLOTCLOCK_SERVO_LIFT, servoLift);
          delayMicroseconds(LIFTSPEED);
        }
      } else {
        while (servoLift <= LIFT_PARKING) {
          servoLift++;
          servo_writeMicroseconds(PLOTCLOCK_SERVO_LIFT, servoLift);
          delayMicroseconds(LIFTSPEED);
        }
      }
      break;

    default:
      break;
  } /* switch */
}

static void MoveToParking(void) {
  lift(PLOTCLOCK_LIFT_POS_PARKING);
  drawTo(PLOTCLOCK_PARKPOS_X, PLOTCLOCK_PARKPOS_Y); /* move to parking position */
}

typedef struct {
  uint16_t ms;
  uint16_t val1, val2;
} VibraSequence;

static VibraSequence vibras[] =
{
  {300, 0xc00, 0xc00},
  {200, 0x500, 0x500},
  {500, 0x800, 0x800},
  {500, 0x900, 0x900},
  {500, 0xa00, 0xa00},
  {500, 0xb00, 0xb00},
  {500, 0xa00, 0xa00},
  {0, 0xfff, 0xfff}, /* off */
};

static void DoSandVibration(void) {
  int i, j;

  MoveToParking();
  for(j=0;j<3;j++) {
    for(i=0;i<sizeof(vibras)/sizeof(vibras[0]);i++) {
      PCA9685_SetChannelPWM(PCA9685_I2C_DEFAULT_ADDR, PLOTCLOCK_VIBRA1, vibras[i].val1);
      PCA9685_SetChannelPWM(PCA9685_I2C_DEFAULT_ADDR, PLOTCLOCK_VIBRA2, vibras[i].val2);
      delay(vibras[i].ms);
    }
  }
}

/* Writing numeral with bx by being the bottom left origin point. Scale 1 equals a 20 mm high font.
   The structure follows this principle: move to first start point of the numeral, lift down, draw numeral, lift up */
static void number(float bx, float by, int num, float scale) {
  switch (num) {
  case 0:
    drawTo(bx + 12.0 * scale, by + 6.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    bogenGZS(bx + 7.0 * scale, by + 10.0 * scale, 10.0 * scale, -0.8, 6.7, 0.5);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 1:
    drawTo(bx + 10.0 * scale, by + 20.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    drawTo(bx + 10.0 * scale, by + 0.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 2:
    drawTo(bx + 2.0 * scale, by + 12.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    bogenUZS(bx + 8.0 * scale, by + 14.0 * scale, 6.0 * scale, 3, -0.8, 1);
    drawTo(bx + 0.0 * scale, by + 0.0 * scale);
    drawTo(bx + 14.0 * scale, by + 0.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 3:
    drawTo(bx + 2.0 * scale, by + 20.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    drawTo(bx + 12.0 * scale, by + 20.0 * scale);
    drawTo(bx + 2.0 * scale, by + 10.0 * scale);
    bogenUZS(bx + 5.0 * scale, by + 5.0 * scale, 5.0 * scale, 1.57, -3, 1);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 4:
    drawTo(bx + 10.0 * scale, by + 2.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    drawTo(bx + 10.0 * scale, by + 22.0 * scale);
    drawTo(bx + 0.0 * scale, by + 8.0 * scale);
    drawTo(bx + 14.0 * scale, by + 8.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 5:
    drawTo(bx + 2.0 * scale, by + 5.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    bogenGZS(bx + 5.0 * scale, by + 6.0 * scale, 6.0 * scale, -2.5, 2, 1);
    drawTo(bx + 3.0 * scale, by + 20.0 * scale);
    drawTo(bx + 12.0 * scale, by + 20.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 6:
    drawTo(bx + 2.0 * scale, by + 10.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    bogenUZS(bx + 7.0 * scale, by + 6.0 * scale, 6.0 * scale, 2, -4.4, 1);
    drawTo(bx + 11.0 * scale, by + 20.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 7:
    drawTo(bx + 2.0 * scale, by + 20.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    drawTo(bx + 12.0 * scale, by + 20.0 * scale);
    drawTo(bx + 2.0 * scale, by + 0.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 8:
    drawTo(bx + 5.0 * scale, by + 10.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    bogenUZS(bx + 5.0 * scale, by + 15.0 * scale, 5.0 * scale, 4.7, -1.6, 1);
    bogenGZS(bx + 5.0 * scale, by + 5.0 * scale, 5.0 * scale, -4.7, 2, 1);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 9:
    drawTo(bx + 9.0 * scale, by + 11.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    bogenUZS(bx + 7.0 * scale, by + 15.0 * scale, 5.0 * scale, 4, -0.5, 1);
    drawTo(bx + 5 * scale, by + 0);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 11: /* special: draw ':' */
    drawTo(bx + 5.0 * scale, by + 15.0 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    delay(200); /* give time to put it down */
    bogenGZS(bx + 5.0 * scale, by + 15.0 * scale, 0.1 * scale, 1, -1, 1);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    drawTo(bx + 5 * scale, by + 5 * scale);
    lift(PLOTCLOCK_LIFT_POS_DRAW);
    delay(200); /* give time to put it down */
    bogenGZS(bx + 5.0 * scale, by + 5.0 * scale, 0.1 * scale, 1, -1, 1);
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    break;
  case 111: /* special: wipe */
    DoSandVibration();
    break;
  }
}

void PlotClock_Setup(void) {
  /* turn vibration motors off */
  PCA9685_SetChannelPWM(PCA9685_I2C_DEFAULT_ADDR, PLOTCLOCK_VIBRA1, 0xfff);
  PCA9685_SetChannelPWM(PCA9685_I2C_DEFAULT_ADDR, PLOTCLOCK_VIBRA2, 0xfff);
  servoLift = LIFT_INIT;
  last_min = 0;
  servo_writeMicroseconds(PLOTCLOCK_SERVO_LIFT, servoLift);
  drawTo(PLOTCLOCK_PARKPOS_X, PLOTCLOCK_PARKPOS_Y); /* move to parking position */
  lift(PLOTCLOCK_LIFT_POS_PARKING); /* move down to surface */
}

static void PlotClockPrintTime(uint8_t hour, uint8_t minute) {
  float digitScale = 1.2;
  int8_t x, y;

  isWriting = TRUE;
  number(0, 0, 111, 0); /* erase */
  x = leftLowerCornerX;
  y = leftLowerCornerY;
  lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
  number(x,    y, hour/10, digitScale); /* write first digit of hour */
  x += 13*digitScale;
  number(x, y, hour%10, digitScale); /* write second digit */
  x += 12*digitScale;
  number(x, y, 11, digitScale); /* draw ':' */
  x += 8*digitScale;
  number(x, y, minute/10, digitScale); /* write most significant digit of hour */
  x += 13*digitScale;
  number(x, y, minute%10, digitScale); /* second digit */
  lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
  MoveToParking();
  isWriting = FALSE;
}

void PlotClock_Loop(void) {
  if (isWriting) {
    return; /* already writing something */
  }
  if (!PlotClockIsEnabled) {
    return; /* clock not enabled */
  }
  if (PlotClockCalibrateIsOn) {
    isWriting = TRUE;
    lift(PLOTCLOCK_LIFT_POS_BETWEEN_DRAW);
    /* servo horns will have 90� between movements, parallel to x and y axis */
    /* drawTo(-3, 29.2):
     * (1): adjust SERVOLEFTNULL
     * (3): adjust SERVOFACTOR_RIGHT
     *              (2)     (3)
     *                       *
     *                       *
     *                       *
     *                       *
     *    (1)*********       *         (4)
     *
     * drawTo(74.1, 28):
     * (2): adjust SERVOFACTOR_LEFT
     * (4): adjust SERVORIGHTNULL
     *              (2)     (3)
     *               *
     *               *
     *               *
     *               *
     *    (1)        *       **********(4)
     */
    drawTo(-3, 29.2); /* left servo horizontal, right servo vertical: adjust SERVOLEFTNULL and SERVOFACTOR_RIGHT */
    delay(500);
    drawTo(74.1, 28); /* left servo vertical, right servo horizontal: adjust SERVOFACTOR_LEFT and SERVORIGHTNULL */
    delay(500);
    isWriting = FALSE;
  } else {
    TIMEREC time;

    TmDt1_GetTime(&time);
    if (last_min != time.Min) {
      PlotClockPrintTime(time.Hour, time.Min);
      last_min = time.Min;
    }
  }
}

static uint8_t PrintStatus(CLS1_ConstStdIOType *io) {
  uint8_t buf[48];

  CLS1_SendStatusStr((unsigned char*)"plotclock", (const unsigned char*)"\r\n", io->stdOut);

  CLS1_SendStatusStr((unsigned char*)"  Enabled", PlotClockIsEnabled?(unsigned char*)"yes\r\n":(unsigned char*)"no\r\n", io->stdOut);
  CLS1_SendStatusStr((unsigned char*)"  Calibrating", PlotClockCalibrateIsOn?(unsigned char*)"yes\r\n":(unsigned char*)"no\r\n", io->stdOut);

  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), SERVOFAKTOR_LEFT); UTIL1_strcat(buf, sizeof(buf), "\r\n");
  CLS1_SendStatusStr((unsigned char*)"  FAKTOR L", buf, io->stdOut);

  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), SERVOFAKTOR_RIGHT); UTIL1_strcat(buf, sizeof(buf), "\r\n");
  CLS1_SendStatusStr((unsigned char*)"  FAKTOR R", buf, io->stdOut);

  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), SERVOLEFTNULL); UTIL1_strcat(buf, sizeof(buf), " us \r\n");
  CLS1_SendStatusStr((unsigned char*)"  LEFTNULL", buf, io->stdOut);

  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), SERVORIGHTNULL); UTIL1_strcat(buf, sizeof(buf), " us\r\n");
  CLS1_SendStatusStr((unsigned char*)"  RIGHTNULL", buf, io->stdOut);

  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), LIFT_DRAW); UTIL1_strcat(buf, sizeof(buf), " us (draw)\r\n");
  CLS1_SendStatusStr((unsigned char*)"  LIFT_DRAW", buf, io->stdOut);
  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), LIFT_BETWEEN_DRAW); UTIL1_strcat(buf, sizeof(buf), " us (between drawing)\r\n");
  CLS1_SendStatusStr((unsigned char*)"  LIFT_BETWEEN_DRAW", buf, io->stdOut);
  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), LIFT_PARKING); UTIL1_strcat(buf, sizeof(buf), " us (parking)\r\n");
  CLS1_SendStatusStr((unsigned char*)"  LIFT_PARKING", buf, io->stdOut);
  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), LIFTSPEED); UTIL1_strcat(buf, sizeof(buf), " us (higher is slower)\r\n");
  CLS1_SendStatusStr((unsigned char*)"  LIFTSPEED", buf, io->stdOut);

  UTIL1_strcpy(buf, sizeof(buf), "left lower corner x:");
  UTIL1_strcatNum16s(buf, sizeof(buf), leftLowerCornerX);
  UTIL1_strcat(buf, sizeof(buf), " y:");
  UTIL1_strcatNum16s(buf, sizeof(buf), leftLowerCornerY);
  UTIL1_strcat(buf, sizeof(buf), "\r\n");
  CLS1_SendStatusStr((unsigned char*)"  base", buf, io->stdOut);


  buf[0] = '\0'; UTIL1_Num16uToStr(buf, sizeof(buf), servoLift); UTIL1_strcat(buf, sizeof(buf), " us\r\n");
  CLS1_SendStatusStr((unsigned char*)"  lift servo", buf, io->stdOut);

  buf[0] = '\0'; UTIL1_NumFloatToStr(buf, sizeof(buf), lastX, 2); UTIL1_strcat(buf, sizeof(buf), "\r\n");
  CLS1_SendStatusStr((unsigned char*)"  last X", buf, io->stdOut);

  buf[0] = '\0'; UTIL1_NumFloatToStr(buf, sizeof(buf), lastY, 2); UTIL1_strcat(buf, sizeof(buf), "\r\n");
  CLS1_SendStatusStr((unsigned char*)"  last Y", buf, io->stdOut);
  return ERR_OK;
}

uint8_t PlotClock_ParseCommand(const unsigned char *cmd, bool *handled, const CLS1_StdIOType *io) {
  int16_t x, y;
  const uint8_t *p;

  if (UTIL1_strcmp((char*)cmd, CLS1_CMD_HELP)==0 || UTIL1_strcmp((char*)cmd, "plotclock help")==0) {
    CLS1_SendHelpStr((unsigned char*)"plotclock", (const unsigned char*)"Group of plotclock commands\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  help|status", (const unsigned char*)"Print help or status information\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  (enable|disable)", (const unsigned char*)"Enables the clock or disables it\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  calibrate (on|off)", (const unsigned char*)"Calibration mode on or off\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  lift <us>", (const unsigned char*)"Lift servo us setting\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  write <hh:mm>", (const unsigned char*)"Write time\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  moveto <x> <y>", (const unsigned char*)"Move to position\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  leftcorner <x> <y>", (const unsigned char*)"Set left lower corner position\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  vibrate", (const unsigned char*)"Vibrate for erasing\r\n", io->stdOut);
    *handled = TRUE;
    return ERR_OK;
  } else if ((UTIL1_strcmp((char*)cmd, CLS1_CMD_STATUS)==0) || (UTIL1_strcmp((char*)cmd, "plotclock status")==0)) {
    *handled = TRUE;
    return PrintStatus(io);
  } else if (UTIL1_strcmp((char*)cmd, "plotclock enable")==0) {
    PlotClockIsEnabled = TRUE;
    *handled = TRUE;
  } else if (UTIL1_strcmp((char*)cmd, "plotclock disable")==0) {
    PlotClockIsEnabled = FALSE;
    *handled = TRUE;
  } else if (UTIL1_strcmp((char*)cmd, "plotclock calibrate on")==0) {
    PlotClockCalibrateIsOn = TRUE;
    *handled = TRUE;
  } else if (UTIL1_strcmp((char*)cmd, "plotclock calibrate off")==0) {
    PlotClockCalibrateIsOn = FALSE;
    *handled = TRUE;
  } else if (UTIL1_strncmp((char*)cmd, "plotclock lift ", sizeof("plotclock lift ")-1)==0) {
    uint16_t us;

    p = cmd + sizeof("plotclock lift ")-1;
    if (UTIL1_ScanDecimal16sNumber(&p, &us)==ERR_OK) {
      servo_writeMicroseconds(PLOTCLOCK_SERVO_LIFT, us);
    }
    *handled = TRUE;
  } else if (UTIL1_strncmp((char*)cmd, "plotclock write ", sizeof("plotclock write ")-1)==0) {
    uint8_t hour, minute, second, hsecond;

    p = cmd + sizeof("plotclock write ")-1;
    if (UTIL1_ScanTime(&p, &hour, &minute, &second, &hsecond)==ERR_OK) {
      PlotClockPrintTime(hour, minute);
    }
    *handled = TRUE;
  } else if (UTIL1_strncmp((char*)cmd, "plotclock leftcorner ", sizeof("plotclock leftcorner ")-1)==0) {
    p = cmd + sizeof("plotclock leftcorner ")-1;
    if (UTIL1_ScanDecimal16sNumber(&p, &x)==ERR_OK && UTIL1_ScanDecimal16sNumber(&p, &y)==ERR_OK) {
      leftLowerCornerX = x;
      leftLowerCornerY = y;
    }
    *handled = TRUE;
  } else if (UTIL1_strncmp((char*)cmd, "plotclock moveto ", sizeof("plotclock moveto ")-1)==0) {
    p = cmd + sizeof("plotclock moveto ")-1;
    if (UTIL1_ScanDecimal16sNumber(&p, &x)==ERR_OK && UTIL1_ScanDecimal16sNumber(&p, &y)==ERR_OK) {
      drawTo(x, y);
    }
    *handled = TRUE;
  } else if (UTIL1_strcmp((char*)cmd, "plotclock vibrate")==0) {
    number(0, 0, 111, 0);
    *handled = TRUE;
  }
  return ERR_OK;
}
