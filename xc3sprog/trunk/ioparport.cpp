/* JTAG GNU/Linux parport device io

Copyright (C) 2004 Andrew Rogers
Additions for Byte Blaster Cable (C) 2005-2011  Uwe Bonnes 
                              bon@elektron.ikp.physik.tu-darmstadt.de

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

Changes:
Dmitry Teytelman [dimtey@gmail.com] 14 Jun 2006 [applied 13 Aug 2006]:
    Code cleanup for clean -Wall compile.
    Changes to support new IOBase interface.
    Support for byte counting and progress bar.
*/


#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>

#ifdef __linux__
// Default parport device
#ifndef PPDEV
#  define PPDEV "/dev/parport0"
#endif

#include <sys/ioctl.h>
#  include <linux/parport.h>
#  include <linux/ppdev.h>
#include <errno.h>

#elif defined (__FreeBSD__)
// Default parport device
#ifndef PPDEV
#  define PPDEV "/dev/parport0"
#include <errno.h>
#endif

#include <sys/ioctl.h>
#  include <dev/ppbus/ppi.h>
#  include <dev/ppbus/ppbconf.h>

#  define PARPORT_CONTROL_STROBE    STROBE
#  define PARPORT_CONTROL_AUTOFD    AUTOFEED
#  define PARPORT_CONTROL_INIT      INIT
#  define PARPORT_CONTROL_SELECT    SELECTIN
                     /* DLC 5 Schematics:
            http://www.xilinx.com/itp/xilinx4/data/docs/pac/appendixb.html
                        Pin Connectes for a 25 pin parallel port connector
                        http://www.kabelfaq.de/-> parallel
                        Pin 15 is nERR*/
#  define PARPORT_STATUS_ERROR      nFAULT
                     /* PIN 13 is SELECT (Printer is online)*/
#  define PARPORT_STATUS_SELECT     SELECT
                     /* PIN 12 is PE */
#  define PARPORT_STATUS_PAPEROUT   PERROR
                     /* PIN 10 is nACK */
#  define PARPORT_STATUS_ACK        nACK
                     /* PIN 11 is nBusy */
#  define PARPORT_STATUS_BUSY       nBUSY

#elif defined (__WIN32__)
// Default parport device
#ifndef PPDEV
#  define PPDEV "\\\\.\\$VDMLPT1"
#endif
#include <windows.h>
#include <ddk/ntddpar.h>
#include "par_nt.h"

/*FIXME: These defines fit numerically, but not logically*/
#  define PARPORT_CONTROL_STROBE    PARALLEL_INIT 
#  define PARPORT_CONTROL_AUTOFD    PARALLEL_AUTOFEED
#  define PARPORT_CONTROL_INIT      PARALLEL_PAPER_EMPTY
#  define PARPORT_CONTROL_SELECT    PARALLEL_OFF_LINE
  
#  define PARPORT_STATUS_ERROR      PARALLEL_OFF_LINE
#  define PARPORT_STATUS_SELECT     PARALLEL_POWER_OFF
#  define PARPORT_STATUS_PAPEROUT   PARALLEL_NOT_CONNECTED
#  define PARPORT_STATUS_ACK        PARALLEL_BUSY
#  define PARPORT_STATUS_BUSY       PARALLEL_SELECTED 
#endif

#include <sys/time.h>
#include <unistd.h>

#include "ioparport.h"
#include "debug.h"

#define NO_CABLE 0
#define IS_PCIII 1
#define IS_BBLST 2

#define BIT_MASK(b)     (1<<(b))

/* Attention: PARPORT_STATUS_BUSY reflects the inverted input */
/* Attention: PARPORT_CONTROL_AUTOFD write zero to output */

/* Altera Byteblaster Definitions */
#define BBLST_DEF_BYTE      0
#define BBLST_ENABLE_N      PARPORT_CONTROL_AUTOFD   /* Base + 2, Inv */
#define BBLST_TCK_VALUE     BIT_MASK(0)              /* Base */
#define BBLST_TMS_VALUE     BIT_MASK(1)              /* Base */
#define BBLST_TDI_VALUE     BIT_MASK(6)              /* Base */
#define BBLST_RESET_VALUE   BIT_MASK(3)              /* Base, Inv by Open 
			    			        Collector Transistor */
#define BBLST_TDO_MASK      PARPORT_STATUS_BUSY      /* Base + 1, Input */
#define BBLST_LB_IN_MASK    PARPORT_STATUS_PAPEROUT  /* Base + 1, Input */
#define BBLST_LB_OUT_VALUE  BIT_MASK(7)              /* Base */
#define BBLST_ACK_OUT_VALUE BIT_MASK(5)
#define BBLST_ACK_IN_MASK   PARPORT_STATUS_ACK

/* Xilinx Parallel Cable III Definitions */
#define PCIII_PROG_EN_N     BIT_MASK(4)
#define PCIII_DEF_BYTE      PCIII_PROG_EN_N
#define PCIII_TCK_VALUE     BIT_MASK(1)              /* Base */
#define PCIII_TMS_VALUE     BIT_MASK(2)              /* Base */
#define PCIII_TDI_VALUE     BIT_MASK(0)              /* Base */
#define PCIII_TDO_MASK      PARPORT_STATUS_SELECT
#define PCIII_CHECK_OUT     BIT_MASK(6)
#define PCIII_CHECK_IN1     PARPORT_STATUS_BUSY
#define PCIII_CHECK_IN2     PARPORT_STATUS_PAPEROUT

							 
using namespace std;

int  IOParport::detectcable(void)
{
  unsigned char data=0, status, control;

  
  write_data(fd, data);
  read_status(fd, &status);
  read_control(fd, &control);
  if ((status == 0) || (status == 0xff))
    {
      fprintf(stderr,"IOParport::detectcable status 0x%02x control %02x"
	      " Check system driver setup\n",
	      status, control);
      return NO_CABLE;
    }
  /* Error_n should is hardwired to ground on a byteblaster cable*/
  if (!(status & PARPORT_STATUS_ERROR))
    {
      if (debug & HW_DETAILS)
	fprintf(stderr,"Trying Byteblaster\n");
      /* D5/ACK and D7/PE should be connected*/
      if (( (data & BBLST_LB_OUT_VALUE)  && !(status & BBLST_LB_IN_MASK)) ||
	  (!(data & BBLST_LB_OUT_VALUE)  &&  (status & BBLST_LB_IN_MASK)))
	{ /* The difference is in D7/PE if the card is unpowered*/
	  if(( (data & BBLST_ACK_OUT_VALUE) &&  (status & BBLST_ACK_IN_MASK))||
	     (!(data & BBLST_ACK_OUT_VALUE) && !(status & BBLST_ACK_IN_MASK)))
	    {
	      fprintf(stderr,"Unpowered Byteblaster cable\n");
	      return NO_CABLE;
	    }
	  /*We have an unpowered Xilinx cable if D6/Busy/PE are connected */
	  else if
	    ( ( (data & PCIII_CHECK_OUT) && !(status & PCIII_CHECK_IN1))||
	      (!(data & PCIII_CHECK_OUT) &&  (status & PCIII_CHECK_IN1))||
	      ( (data & PCIII_CHECK_OUT) &&  (status & PCIII_CHECK_IN2))||
	      (!(data & PCIII_CHECK_OUT) && !(status & PCIII_CHECK_IN2))) {
	    fprintf(stderr,"Unpowered Parallel Cable III cable\n");
	    return NO_CABLE;
	  }
	  else 	    {
	    fprintf(stderr,"No dongle found\n");
	    return NO_CABLE;
	  }
	}
      /* now try all 4 permuttation */
      data = (data & BBLST_LB_OUT_VALUE) ? (data & ~BBLST_LB_OUT_VALUE) : 
	(data | BBLST_LB_OUT_VALUE);
      write_data(fd, data);
      read_status(fd, &status);
      if (( (data & BBLST_LB_OUT_VALUE)  && !(status & BBLST_LB_IN_MASK)) ||
	  (!(data & BBLST_LB_OUT_VALUE)  &&  (status & BBLST_LB_IN_MASK)) ||
	  ( (data & BBLST_ACK_OUT_VALUE) && !(status & BBLST_ACK_IN_MASK))||
	  (!(data & BBLST_ACK_OUT_VALUE) &&  (status & BBLST_ACK_IN_MASK)))
	{
	  fprintf(stderr,"Missing reaction for Altera cable(1)\n");
	  return NO_CABLE;
	}
      data = (data & BBLST_ACK_OUT_VALUE) ? (data & ~BBLST_ACK_OUT_VALUE) :
	(data | BBLST_ACK_OUT_VALUE);
      write_data(fd, data);
      read_status(fd, &status);
      if (( (data & BBLST_LB_OUT_VALUE)  && !(status & BBLST_LB_IN_MASK)) ||
	  (!(data & BBLST_LB_OUT_VALUE)  &&  (status & BBLST_LB_IN_MASK)) ||
	  ( (data & BBLST_ACK_OUT_VALUE) && !(status & BBLST_ACK_IN_MASK))||
	  (!(data & BBLST_ACK_OUT_VALUE) &&  (status & BBLST_ACK_IN_MASK)))
	{
	  fprintf(stderr,"Missing reaction for Altera cable(2)\n");
	  return NO_CABLE;
	}
      data = (data & BBLST_LB_OUT_VALUE) ? (data & ~BBLST_LB_OUT_VALUE) :
	(data | BBLST_LB_OUT_VALUE);
      write_data(fd, data);
      read_status(fd, &status);
      if (( (data & BBLST_LB_OUT_VALUE)  && !(status & BBLST_LB_IN_MASK)) ||
	  (!(data & BBLST_LB_OUT_VALUE)  &&  (status & BBLST_LB_IN_MASK)) ||
	  ( (data & BBLST_ACK_OUT_VALUE) && !(status & BBLST_ACK_IN_MASK))||
	  (!(data & BBLST_ACK_OUT_VALUE) &&  (status & BBLST_ACK_IN_MASK)))
	{
	  fprintf(stderr,"Missing reaction for Altera cable(3)\n");
	  return NO_CABLE;
	}
      data = (data & BBLST_ACK_OUT_VALUE) ? (data & ~BBLST_ACK_OUT_VALUE) :
	(data | BBLST_ACK_OUT_VALUE);
      write_data(fd, data);
      read_status(fd, &status);
      if (( (data & BBLST_LB_OUT_VALUE)  && !(status & BBLST_LB_IN_MASK)) ||
	  (!(data & BBLST_LB_OUT_VALUE)  &&  (status & BBLST_LB_IN_MASK)) ||
	  ( (data & BBLST_ACK_OUT_VALUE) && !(status & BBLST_ACK_IN_MASK))||
	  (!(data & BBLST_ACK_OUT_VALUE) &&  (status & BBLST_ACK_IN_MASK)))
	{
	  fprintf(stderr,"Missing reaction for Altera cable(4)\n");
	  return NO_CABLE;
	}
      fprintf(stderr,"Found ByteBlaster Cable\n");
      def_byte = BBLST_DEF_BYTE;
      tdi_value = BBLST_TDI_VALUE;
      tms_value = BBLST_TMS_VALUE;
      tck_value = BBLST_TCK_VALUE;
      tdo_mask = BBLST_TDO_MASK;
      tdo_inv  = 1;
      read_control(fd, &control);
      control |=  BBLST_ENABLE_N;
      write_control(fd, control);
      return IS_BBLST;
    }
  else { /*Probably  Xilinx cable */
    /* Check for D6/BUSY/PE Connection and for D4/Select Feedback */
    if ( ( (data & PCIII_CHECK_OUT) &&  (status & PCIII_CHECK_IN1))||
	 (!(data & PCIII_CHECK_OUT) && !(status & PCIII_CHECK_IN1))||
	 ( (data & PCIII_CHECK_OUT) && !(status & PCIII_CHECK_IN2))||
	 (!(data & PCIII_CHECK_OUT) &&  (status & PCIII_CHECK_IN2)))
      {
	fprintf(stderr,"No dongle found\n");
	return NO_CABLE;
      }

/* 20100708: This check will only work with U1 on the DLC(clone) 
 * from a high drive family like LVC and a not so strong driver at the end
 * of the JTAG chain, like an XCF0x.
 * E.G. Digilent S3 drives TDO-A with an LVC, while the
 * original DLC5 only has an HC125, and so the HC125 can not drive the line
 * low consitstantly
 *
 * So disable this check

    if ((status & PCIII_TDO_MASK) && (!(data & PCIII_PROG_EN_N))) {
	fprintf(stderr,"Missing power for Parallel Cable III\n");
	return NO_CABLE;}
*/
    data = (data & PCIII_CHECK_OUT) ? (data & ~PCIII_CHECK_OUT) :
      (data |PCIII_CHECK_OUT);
    write_data(fd, data);
    read_status(fd, &status);
    if ( ( (data & PCIII_CHECK_OUT) &&  (status & PCIII_CHECK_IN1))||
	 (!(data & PCIII_CHECK_OUT) && !(status & PCIII_CHECK_IN1))||
	 ( (data & PCIII_CHECK_OUT) && !(status & PCIII_CHECK_IN2)) ||
	 (!(data & PCIII_CHECK_OUT) && (status & PCIII_CHECK_IN2)))
	{
	  fprintf(stderr,"Missing reaction on XILINX Cable(1)\n");
	  return NO_CABLE;
	}
    data = (data & PCIII_CHECK_OUT) ? (data & ~PCIII_CHECK_OUT) :
      (data | PCIII_CHECK_OUT);
    write_data(fd, data);
    read_status(fd, &status);
    if ( ( (data & PCIII_CHECK_OUT) &&  (status & PCIII_CHECK_IN1))||
	 (!(data & PCIII_CHECK_OUT) && !(status & PCIII_CHECK_IN1))||
	 ( (data & PCIII_CHECK_OUT) && !(status & PCIII_CHECK_IN2))||
	 (!(data & PCIII_CHECK_OUT) && (status & PCIII_CHECK_IN2)))
      {
	fprintf(stderr,"Missing reaction on XILINX Cable(2)\n");
	return NO_CABLE;
      }
    fprintf(stderr,"Found Xilinx Parallel Cable III\n");
    def_byte = PCIII_DEF_BYTE;
    tdi_value = PCIII_TDI_VALUE;
    tms_value = PCIII_TMS_VALUE;
    tck_value = PCIII_TCK_VALUE;
    tdo_mask = PCIII_TDO_MASK;
    tdo_inv = 0;
    return IS_PCIII;
  }
}

IOParport::IOParport() : IOBase(), total(0), debug(0) 
{
}

int IOParport::Init(struct cable_t *cable, const char *dev, unsigned int freq)
{
    int res;

  // Try to obtain device from environment or use default if not given
  if(!dev) {
    if(!(dev = getenv("XCPORT")))  dev = PPDEV;
  }

#if defined (__linux__) || defined(__FreeBSD__)
  // Try to open parport device
  if((fd = open(dev, O_RDWR)) == -1) 
#elif defined(__WIN32__)
    if ((fd = (int)CreateFile(dev, GENERIC_READ | GENERIC_WRITE,
			      0, NULL, OPEN_EXISTING, 0, NULL)) 
	== (int)INVALID_HANDLE_VALUE)
#endif
      {
	fprintf(stderr,"Could not access parallel device '%s': %s\n",
		dev, strerror(errno));
	return -1;
      }
  
#if defined (__linux__)
  // Lock port
  res = ioctl(fd, PPCLAIM);
  if(res) 
  {
      fprintf(stderr, "Port %s already in use\n", dev);
      return res;
  }
  
  // Switch to compatibility mode
  int const  mode = IEEE1284_MODE_COMPAT;
  res = ioctl(fd, PPNEGOT, &mode);
  if(res) {
      fprintf(stderr,"IEEE1284 compatibility not available on dev %s\n", dev);
      return res;
  }
#endif
  cabletype = detectcable();
  if(cabletype == NO_CABLE)
  {
      fprintf(stderr, "No adapter found\n");
      return 1;
  }
  return 0;
}

bool IOParport::txrx(bool tms, bool tdi)
{
  unsigned char ret;
  bool retval;
  unsigned char data=def_byte; // D4 pin5 TDI enable
  if(tdi)data|=tdi_value; // D0 pin2
  if(tms)data|=tms_value; // D2 pin4
  write_data(fd, data);
  data|=tck_value; // clk high D1 pin3
  write_data(fd, data);
  total++;
  read_status(fd, &ret);
  //data=data^2; // clk low
  //write_data(fd, data);
  //read_status(fd, &ret);
  retval = (ret&tdo_mask)?!tdo_inv:tdo_inv;
  if (debug & HW_FUNCTIONS)
    fprintf(stderr,"IOParport::txrx tms %s tdi %s tdo %s \n",
	    (tms)?"true ":"false", (tdi)?"true ":"false",
	    (retval)?"true ":"false");
  return retval; 
    
}

void IOParport::tx(bool tms, bool tdi)
{
  unsigned char data=def_byte; // D4 pin5 TDI enable
  if (debug & HW_FUNCTIONS)
    fprintf(stderr,"tx tms %s tdi %s\n",(tms)?"true ":"false",
	    (tdi)?"true ":"false");
  if(tdi)data|=tdi_value; // D0 pin2
  if(tms)data|=tms_value; // D2 pin4
  write_data(fd, data);
  //delay(2);
  data|=tck_value; // clk high 
  total++;
  write_data(fd, data);
  //delay(2);
  //data=data^2; // clk low
  //write_data(fd, data);
  //delay(2);
}
 
void IOParport::tx_tdi_byte(unsigned char tdi_byte)
{
  int k;
  
  for (k = 0; k < 8; k++)
    tx(false, (tdi_byte>>k)&1);
}
 
void IOParport::txrx_block(const unsigned char *tdi, unsigned char *tdo,
			   int length, bool last)
{
  int i=0;
  int j=0;
  unsigned char tdo_byte=0;
  unsigned char tdi_byte;
  unsigned char data=def_byte;
  if (tdi)
      tdi_byte = tdi[j];
      
  while(i<length-1){
      tdo_byte=tdo_byte+(txrx(false, (tdi_byte&1)==1)<<(i%8));
      if (tdi)
	  tdi_byte=tdi_byte>>1;
    i++;
    if((i%8)==0){ // Next byte
	if(tdo)
	    tdo[j]=tdo_byte; // Save the TDO byte
      tdo_byte=0;
      j++;
      if (tdi)
	  tdi_byte=tdi[j]; // Get the next TDI byte
    }
  };
  tdo_byte=tdo_byte+(txrx(last, (tdi_byte&1)==1)<<(i%8)); 
  if(tdo)
      tdo[j]=tdo_byte;
  write_data(fd, data); /* Make sure, TCK is low */
  return;
}

void IOParport::tx_tms(unsigned char *pat, int length, int force)
{
    int i;
    unsigned char tms;
    unsigned char data=def_byte;
    for (i = 0; i < length; i++)
    {
      if ((i & 0x7) == 0)
	tms = pat[i>>3];
      tx((tms & 0x01), true);
      tms = tms >> 1;
    }
    write_data(fd, data); /* Make sure, TCK is low */
}

IOParport::~IOParport()
{
  if (cabletype == IS_BBLST)
    {
      unsigned char control;
      read_control(fd, &control);
      control &=  ~BBLST_ENABLE_N;
      write_control(fd, control);
    }
#ifdef __linux__
  ioctl (fd, PPRELEASE);
  close (fd);
#elif defined(__FreeBSD__)
  close (fd);
#elif defined(__WIN32__)
  CloseHandle((HANDLE)(fd));
#endif
  if (verbose) fprintf(stderr, "Total bytes sent: %d\n", total>>3);
}
#define XC3S_OK 0
#define XC3S_EIO 1
#define XC3S_ENIMPL 2

int IOParport::write_data(int fd, unsigned char data)
{
    int status;
#ifdef __linux__
    status = ioctl(fd, PPWDATA, &data);
    return  status == 0 ? XC3S_OK : -XC3S_EIO;
#elif defined (__FreeBSD__)
    status = ioctl(fd, PPISDATA, &data);
    return status == 0 ? XC3S_OK : -XC3S_EIO;
#elif defined(__WIN32__)
    DWORD dummy;
    status = DeviceIoControl((HANDLE)(fd), NT_IOCTL_DATA, &data, sizeof(data), 
                             NULL, 0, (LPDWORD)&dummy, NULL);
    return status != 0 ? XC3S_OK : -XC3S_EIO;
#else
    return -XC3S_ENIMPL;
#endif
}


int IOParport::write_control(int fd, unsigned char control)
{
    int status;
#ifdef __linux__
    status = ioctl(fd, PPWCONTROL, &control);
    return status == 0 ? XC3S_OK : -XC3S_EIO;
#elif defined (__FreeBSD__)
    status = ioctl(fd, PPISCTRL, &control);
    return status == 0 ? XC3S_OK : -XC3S_EIO;
#elif defined(__WIN32__)
    DWORD dummyc;
    DWORD dummy;
    /*FIXME: hamlib used much more compicated expression*/
    status = DeviceIoControl((HANDLE)(fd),NT_IOCTL_CONTROL, &control,
                             sizeof(control), &dummyc, sizeof(dummyc),
			     (LPDWORD)&dummy, NULL);
    return status != 0 ? XC3S_OK : -XC3S_EIO;
#else
    return -XC3S_ENIMPL;
#endif
}

int IOParport::read_control(int fd, unsigned char *control)
{
    int status;
#ifdef __linux
    status = ioctl(fd, PPRCONTROL, control);
    return status == 0 ? XC3S_OK : -XC3S_EIO;
#elif defined (__FreeBSD__)
    status = ioctl(fd, PPIGCTRL, control);
    return status == 0 ? XC3S_OK : -XC3S_EIO;
#elif defined (__WIN32__)
    char ret;
    DWORD dummy;
    status = DeviceIoControl((HANDLE)(fd), NT_IOCTL_CONTROL, NULL, 0, &ret, 
                             sizeof(ret), (LPDWORD)&dummy, NULL);
    *control = ret ^ S1284_INVERTED;
    return status == 0 ? XC3S_OK : -XC3S_EIO;
#else
    return -XC3S_ENIMPL;
#endif
}

int IOParport::read_status(int fd, unsigned char *status)
{
    int ret;
#ifdef __linux__
    ret = ioctl(fd, PPRSTATUS, status);
    return ret == 0 ? XC3S_OK : -XC3S_EIO;
#elif defined (__FreeBSD__)
    ret = ioctl(fd, PPIGSTATUS, status);
    return ret == 0 ? XC3S_OK : -XC3S_EIO;
#elif defined (__WIN32__)
    unsigned char res;
    DWORD dummy;
    ret = DeviceIoControl((HANDLE)(fd), NT_IOCTL_STATUS, NULL, 0, &res, 
                             sizeof(res), (LPDWORD)&dummy, NULL);
    *status = res ;
    return ret == 0 ? XC3S_OK : -XC3S_EIO;
#else
    return -XC3S_ENIMPL;
#endif
}

	
