//*****************************************************************************
//
// eeprom.c - Driver for programming the on-chip EEPROM.
//
// Copyright (c) 2010-2011 Texas Instruments Incorporated.  All rights reserved.
// Software License Agreement
// 
// Texas Instruments (TI) is supplying this software for use solely and
// exclusively on TI's microcontroller products. The software is owned by
// TI and/or its suppliers, and is protected under applicable copyright
// laws. You may not combine this software with "viral" open-source
// software in order to form a larger program.
// 
// THIS SOFTWARE IS PROVIDED "AS IS" AND WITH ALL FAULTS.
// NO WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING, BUT
// NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE. TI SHALL NOT, UNDER ANY
// CIRCUMSTANCES, BE LIABLE FOR SPECIAL, INCIDENTAL, OR CONSEQUENTIAL
// DAMAGES, FOR ANY REASON WHATSOEVER.
// 
// This is part of revision 8034 of the Stellaris Peripheral Driver Library.
//
//*****************************************************************************

#include "inc/hw_eeprom.h"
#include "inc/hw_flash.h"
#include "inc/hw_ints.h"
#include "inc/hw_sysctl.h"
#include "inc/hw_types.h"
#include "driverlib/debug.h"
#include "driverlib/flash.h"
#include "driverlib/interrupt.h"
#include "driverlib/sysctl.h"
#include "driverlib/eeprom.h"

//*****************************************************************************
//
//! \addtogroup eeprom_api
//! @{
//
//*****************************************************************************

//*****************************************************************************
//
// Useful macros to extract the number of EEPROM blocks available on the target
// device and the total EEPROM storage in bytes from the EESIZE register.
//
//*****************************************************************************
#define BLOCKS_FROM_EESIZE(x) (((x) & EEPROM_EESIZE_BLKCNT_M) >>               \
                              EEPROM_EESIZE_BLKCNT_S)
#define SIZE_FROM_EESIZE(x)   ((((x) & EEPROM_EESIZE_WORDCNT_M) >>             \
                              EEPROM_EESIZE_WORDCNT_S) * 4)

//*****************************************************************************
//
// Useful macro to extract the offset from a linear address.
//
//*****************************************************************************
#define OFFSET_FROM_ADDR(x) (((x) >> 2) & 0x0F)

//*****************************************************************************
//
// The key value required to initiate a mass erase.
//
//*****************************************************************************
#define EEPROM_MASS_ERASE_KEY ((unsigned long)0xE37B << EEPROM_EEDBGME_KEY_S)

//*****************************************************************************
//
// This function implements a workaround for a bug in Blizzard rev A silicon.
// It ensure that only the 1KB flash sector containing a given EEPROM address
// is erased if an erase/copy operation is required as a result of a following
// EEPROM write.
//
//*****************************************************************************
static void
EEPROMSetSectorMask(unsigned long ulAddress)
{
    unsigned long ulMask;

    //
    // Determine which page contains the passed EEPROM address.  The 2KB EEPROM
    // is implemented in 16KB of flash with each 1KB sector of flash holding
    // values for 32 consecutive EEPROM words (or 128 bytes).
    //
    ulMask = ~(1 << (ulAddress >> 7));

    SysCtlDelay(10);
    HWREG(0x400FD0FC) = 3;
    SysCtlDelay(10);
    HWREG(0x400AE2C0) = ulMask;
    SysCtlDelay(10);
    HWREG(0x400FD0FC) = 0;
    SysCtlDelay(10);
}

//*****************************************************************************
//
// Clear the FSM sector erase mask to ensure that any following main array flash
// erase operations will operate as expected.
//
//*****************************************************************************
static void
EEPROMClearSectorMask(void)
{
    SysCtlDelay(10);
    HWREG(0x400FD0FC) = 3;
    SysCtlDelay(10);
    HWREG(0x400AE2C0) = 0;
    SysCtlDelay(10);
    HWREG(0x400FD0FC) = 0;
    SysCtlDelay(10);
}

//*****************************************************************************
//
// Block until the EEPROM peripheral is not busy.
//
//*****************************************************************************
static void
EEPROMWaitForDone(void)
{
    //
    // Is the EEPROM still busy?
    //
    while(HWREG(EEPROM_EEDONE) & EEPROM_EEDONE_WORKING)
    {
        //
        // Spin while EEPROM is busy.
        //
    }
}

//*****************************************************************************
//
//! Performs any necessary recovery in case of power failures during write.
//!
//! This function must be called after SysCtlPeripheralEnable() and before
//! the EEPROM is accessed to check for errors resulting from power failure
//! during a previous write operation.  The function will detect these errors
//! and perform as much recovery as possible before returning information to
//! the caller on whether or not a previous data write was lost and will need
//! to be retried.
//!
//! In cases where \b EEPROM_INIT_RETRY is returned, the application is
//! responsible for determining which data write may have been lost and
//! rewriting this data.  If \b EEPROM_INIT_ERROR is returned, this indicates
//! that the EEPROM was unable to recover its state.  This condition may or
//! may not be resolved on future resets depending upon the cause of the
//! fault. For example, if the supply voltage is unstable, retrying the
//! operation once the voltage is stabilized may clear the error.
//!
//! Failure to call this function after a reset may lead to permanent data loss
//! if the EEPROM is later written!
//!
//! \return Returns \b EEPROM_INIT_OK if no errors were detected,
//! \b EEPROM_INIT_RETRY if a previous write operation may have been
//! interrupted by a power or reset event or \b EEPROM_INIT_ERROR if the EEPROM
//! peripheral cannot currently recover from an interrupted write or erase
//! operation.
//
//*****************************************************************************
unsigned long
EEPROMInit(void)
{
    unsigned long ulStatus;

    //
    // Insert a small delay (6 cycles + call overhead) to guard against the
    // possibility that this function is called immediately after the EEPROM
    // peripheral is enabled.  Without this delay, there is a slight chance
    // that the first EEPROM register read will fault if you are using a
    // compiler with a ridiculously good optimizer!
    //
    SysCtlDelay(2);

    //
    // Make sure the EEPROM has finished its reset processing.
    //
    EEPROMWaitForDone();

    //
    // Read the EESUPP register to see if any errors have been reported.
    //
    ulStatus = HWREG(EEPROM_EESUPP);

    //
    // Did an error of some sort occur during a previous attempt to write to
    // the EEPROM?
    //
    if(ulStatus & (EEPROM_EESUPP_PRETRY | EEPROM_EESUPP_ERETRY))
    {
        //
        // Perform a second reset to allow the EEPROM a chance to correct
        // the errors.
        //
        SysCtlPeripheralReset(SYSCTL_PERIPH_EEPROM0);

        //
        // Wait for the EEPROM to complete it's reset processing once again.
        //
        SysCtlDelay(2);
        EEPROMWaitForDone();

        //
        // Read EESUPP once again to determine if the error conditions are
        // cleared.
        //
        ulStatus = HWREG(EEPROM_EESUPP);
        if(ulStatus & (EEPROM_EESUPP_PRETRY | EEPROM_EESUPP_ERETRY))
        {
            return(EEPROM_INIT_ERROR);
        }
        else
        {
            return(EEPROM_INIT_RETRY);
        }
    }

    //
    // The EEPROM does not indicate that any error occurred.
    //
    return(EEPROM_INIT_OK);
}

//*****************************************************************************
//
//! Determines the size of the EEPROM.
//!
//! This function returns the size of the EEPROM in bytes.
//!
//! \return Returns the total number of bytes in the EEPROM.
//
//*****************************************************************************
unsigned long
EEPROMSizeGet(void)
{
    //
    // Return the size of the EEPROM in bytes.
    //
    return(SIZE_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
}

//*****************************************************************************
//
//! Determines the number of blocks in the EEPROM.
//!
//! This function may be called to determine the number of blocks in the
//! EEPROM.  Each block is the same size and the number of bytes of storage
//! contained in a block may be determined by dividing the size of the device,
//! obtained via a call to the EEPROMSizeGet() function, by the number of
//! blocks returned by this function.
//!
//! \return Returns the total number of bytes in the device EEPROM.
//
//*****************************************************************************
unsigned long
EEPROMBlockCountGet(void)
{
    //
    // Extract the number of blocks and return it to the caller.
    //
    return(BLOCKS_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
}

//*****************************************************************************
//
//! Reads data from the EEPROM.
//!
//! \param pulData is a pointer to storage for the data read from the EEPROM.
//! This must point to at least \e ulCount bytes of available memory.
//! \param ulAddress is the byte address within the EEPROM from which data is
//! to be read.  This value must be a multiple of 4.
//! \param ulCount is the number of bytes of data to read from the EEPROM.
//! This value must be a multiple of 4.
//!
//! This function may be called to read a number of words of data from a
//! word-aligned address within the EEPROM.  Data read is copied into the
//! buffer pointed to by the \e pulData parameter.
//!
//! \return None.
//
//*****************************************************************************
void
EEPROMRead(unsigned long *pulData, unsigned long ulAddress,
           unsigned long ulCount)
{
    //
    // Check parameters in a debug build.
    //
    ASSERT(pulData);
    ASSERT(ulAddress < SIZE_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
    ASSERT((ulAddress + ulCount) <= SIZE_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
    ASSERT((ulAddress & 3) == 0);
    ASSERT((ulCount & 3) == 0);

    //
    // Set the block and offset appropriately to read the first word.
    //
    HWREG(EEPROM_EEBLOCK) = EEPROMBlockFromAddr(ulAddress);
    HWREG(EEPROM_EEOFFSET) = OFFSET_FROM_ADDR(ulAddress);

    //
    // Convert the byte count to a word count.
    //
    ulCount /= 4;

    //
    // Read each word in turn.
    //
    while(ulCount)
    {
        //
        // Read the next word through the autoincrementing register.
        //
        *pulData = HWREG(EEPROM_EERDWRINC);

        //
        // Move on to the next word.
        //
        pulData++;
        ulCount--;

        //
        // Do we need to move to the next block?  This is the case if the
        // offset register has just wrapped back to 0.
        //
        if(HWREG(EEPROM_EEOFFSET) == 0)
        {
            HWREG(EEPROM_EEBLOCK) += 1;
        }
    }
}

//*****************************************************************************
//
//! Writes data to the EEPROM.
//!
//! \param pulData points to the first word of data to write to the EEPROM.
//! \param ulAddress defines the byte address within the EEPROM that the data
//! is to be written to.  This value must be a multiple of 4.
//! \param ulCount defines the number of bytes of data that is to be written.
//! This value must be a multiple of 4.
//!
//! This function may be called to write data into the EEPROM at a given
//! word-aligned address.  The call is synchronous and returns only after
//! all data has been written or an error occurs.
//!
//! \return Returns 0 on success or non-zero values on failure.  Failure codes
//! are logical OR combinations of \b EEPROM_RC_INVPL, \b EEPROM_RC_WRBUSY,
//! \b EEPROM_RC_NOPERM, \b EEPROM_RC_WKCOPY, \b EEPROM_RC_WKERASE, and
//! \b EEPROM_RC_WORKING.
//
//*****************************************************************************
unsigned long
EEPROMProgram(unsigned long *pulData, unsigned long ulAddress,
              unsigned long ulCount)
{
    unsigned long ulStatus;

    //
    // Check parameters in a debug build.
    //
    ASSERT(pulData);
    ASSERT(ulAddress < SIZE_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
    ASSERT((ulAddress + ulCount) <= SIZE_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
    ASSERT((ulAddress & 3) == 0);
    ASSERT((ulCount & 3) == 0);

    //
    // This is a workaround for a silicon problem on Blizzard rev A.
    //
    if(CLASS_IS_BLIZZARD && REVISION_IS_A0)
    {
        EEPROMSetSectorMask(ulAddress);
    }

    //
    // Set the block and offset appropriately to program the first word.
    //
    HWREG(EEPROM_EEBLOCK) = EEPROMBlockFromAddr(ulAddress);
    HWREG(EEPROM_EEOFFSET) = OFFSET_FROM_ADDR(ulAddress);

    //
    // Convert the byte count to a word count.
    //
    ulCount /= 4;

    //
    // Write each word in turn.
    //
    while(ulCount)
    {
        //
        // Write the next word through the autoincrementing register.
        //
        HWREG(EEPROM_EERDWRINC) = *pulData;

        //
        // Wait for the write to complete.
        //
        do
        {
            //
            // Read the status.
            //
            ulStatus = HWREG(EEPROM_EEDONE);
        }
        while(ulStatus & EEPROM_EEDONE_WORKING);

        //
        // Make sure we completed the write without errors.  Note that we
        // must check this per-word since write permission can be set per
        // block resulting in only a section of the write not being performed.
        //
        if(ulStatus & (EEPROM_EEDONE_NOPERM | EEPROM_EEDONE_INVPL))
        {
            //
            // An error was reported that would prevent the values from
            // being written correctly.
            //
            if(CLASS_IS_BLIZZARD && REVISION_IS_A0)
            {
                EEPROMClearSectorMask();
            }
            return(ulStatus);
        }

        //
        // Move on to the next word.
        //
        pulData++;
        ulCount--;

        //
        // Do we need to move to the next block?  This is the case if the
        // offset register has just wrapped back to 0.
        //
        if(HWREG(EEPROM_EEOFFSET) == 0)
        {
            HWREG(EEPROM_EEBLOCK) += 1;
        }
    }

    //
    // Clear the sector protection bits to prevent possible problems when
    // programming the main flash array later.
    //
    if(CLASS_IS_BLIZZARD && REVISION_IS_A0)
    {
        EEPROMClearSectorMask();
    }

    //
    // Return the current status to the caller.
    //
    return(HWREG(EEPROM_EEDONE));
}

//*****************************************************************************
//
//! Writes a word to the EEPROM.
//!
//! \param ulData is the word to write to the EEPROM.
//! \param ulAddress defines the byte address within the EEPROM to which the
//! data is to be written.  This value must be a multiple of 4.
//!
//! This function is intended to allow EEPROM programming under interrupt
//! control. It may be called to start the process of writing a single word of
//! data into the EEPROM at a given word-aligned address.  The call is
//! asynchronous and returna immediately without waiting for the write to
//! complete.  Completion of the operation is signaled by means of an
//! interrupt from the EEPROM module.  The EEPROM peripheral shares a single
//! interrupt vector with the flash memory subsystem, \e INT_FLASH.
//!
//! \return Returns status and error information in the form of a logical OR
//! combinations of \b EEPROM_RC_INVPL, \b EEPROM_RC_WRBUSY,
//! \b EEPROM_RC_NOPERM, \b EEPROM_RC_WKCOPY, \b EEPROM_RC_WKERASE and
//! \b EEPROM_RC_WORKING.  Flags \b EEPROM_RC_WKCOPY, \b EEPROM_RC_WKERASE, and
//! \b EEPROM_RC_WORKING are expected in normal operation and do not indicate
//! an error.
//
//*****************************************************************************
unsigned long
EEPROMProgramNonBlocking(unsigned long ulData, unsigned long ulAddress)
{
    //
    // Check parameters in a debug build.
    //
    ASSERT(ulAddress < SIZE_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
    ASSERT((ulAddress & 3) == 0);

    //
    // This is a workaround for a silicon problem on Blizzard rev A.
    //
    if(CLASS_IS_BLIZZARD && REVISION_IS_A0)
    {
        EEPROMSetSectorMask(ulAddress);
    }

    //
    // Set the block and offset appropriately to program the desired word.
    //
    HWREG(EEPROM_EEBLOCK) = EEPROMBlockFromAddr(ulAddress);
    HWREG(EEPROM_EEOFFSET) = OFFSET_FROM_ADDR(ulAddress);

    //
    // Write the new word using the auto-incrementing register just in case
    // the caller wants to write follow-on words using direct register access
    //
    HWREG(EEPROM_EERDWRINC) = ulData;

    //
    // Return the current status to the caller.
    //
    return(HWREG(EEPROM_EEDONE));
}

//*****************************************************************************
//
//! Erases the EEPROM and returns it to the factory default condition.
//!
//! This function will completely erase the EEPROM and remove any and
//! all access protection on its blocks, leaving the device in the factory
//! default condition.  After this operation, all EEPROM words contain the
//! value 0xFFFFFFFF and all blocks are accessible for both read and write
//! operations in all CPU modes.  No passwords are active.
//!
//! The function is synchronous and does not return until the erase operation
//! has completed.
//!
//! \return Returns 0 on success or non-zero values on failure.  Failure codes
//! are logical OR combinations of \b EEPROM_RC_INVPL, \b EEPROM_RC_WRBUSY,
//! \b EEPROM_RC_NOPERM, \b EEPROM_RC_WKCOPY, \b EEPROM_RC_WKERASE, and
//! \b EEPROM_RC_WORKING.
//
//*****************************************************************************
unsigned long
EEPROMMassErase(void)
{
    //
    // Start the mass erase processing
    //
    HWREG(EEPROM_EEDBGME) = EEPROM_MASS_ERASE_KEY | EEPROM_EEDBGME_ME;

    //
    // Wait for completion.
    //
    EEPROMWaitForDone();

    //
    // Reset the peripheral.  This si required so that all protection
    // mechanisms and passwords are reset now that the EEPROM data has been
    // scrubbed.
    //
    SysCtlPeripheralReset(SYSCTL_PERIPH_EEPROM0);

    //
    // Wait for completion again.
    //
    SysCtlDelay(2);
    EEPROMWaitForDone();

    //
    // Pass any error codes back to the caller.
    //
    return(HWREG(EEPROM_EEDONE));
}

//*****************************************************************************
//
//! Returns the current protection level for an EEPROM block.
//!
//! \param ulBlock is the block number for which the protection level is to be
//! queried.
//!
//! This function returns the current protection settings for a given
//! EEPROM block.  If block 0 is currently locked, it must be unlocked prior
//! to calling this function to query the protection setting for other blocks.
//!
//! \return Returns one of \b EEPROM_PROT_RW_LRO_URW, \b EEPROM_PROT_NA_LNA_URW
//! or \b EEPROM_PROT_RO_LNA_URO optionally OR-ed with
//! \b EEPROM_PROT_SUPERVISOR_ONLY.
//
//*****************************************************************************
unsigned long
EEPROMBlockProtectGet(unsigned long ulBlock)
{
    //
    // Parameter validity check.
    //
    ASSERT(ulBlock < BLOCKS_FROM_EESIZE(HWREG(EEPROM_EESIZE)));

    //
    // Set the current block.
    //
    HWREG(EEPROM_EEBLOCK) = ulBlock;

    //
    // Return the protection flags for this block.
    //
    return(HWREG(EEPROM_EEPROT));
}

//*****************************************************************************
//
//! Set the current protection options for an EEPROM block.
//!
//! \param ulBlock is the block number for which the protection options are to
//! be set.
//! \param ulProtect consists of one of the values \b EEPROM_PROT_RW_LRO_URW,
//! \b EEPROM_PROT_NA_LNA_URW or \b EEPROM_PROT_RO_LNA_URO optionally ORed with
//! \b EEPROM_PROT_SUPERVISOR_ONLY.
//!
//! This function sets the protection settings for a given EEPROM block
//! assuming no protection settings have previously been written.  Note that
//! protection settings applied to block 0 have special meaning and control
//! access to the EEPROM peripheral as a whole.  Protection settings applied to
//! blocks numbered 1 and above are layered above any protection set on block 0
//! such that the effective protection on each block is the logical OR of the
//! protection flags set for block 0 and for the target block.  This allows
//! global protection options to be set for the whole device via block 0 and
//! more restrictive protection settings to be set on a block-by-block basis.
//!
//! The protection flags indicate access permissions as follow:
//!
//! \b EEPROM_PROT_SUPERVISOR_ONLY restricts access to the block to threads
//! running in supervisor mode.  If clear, both user and supervisor threads
//! can access the block.
//!
//! \b EEPROM_PROT_RW_LRO_URW provides read/write access to the block if no
//! password is set or if a password is set and the block is unlocked.  If the
//! block is locked, only read access is permitted.
//!
//! \b EEPROM_PROT_NA_LNA_URW provides neither read nor write access unless
//! a password is set and the block is unlocked.  If the block is unlocked,
//! both read and write access are permitted.
//!
//! \b EEPROM_PROT_RO_LNA_URO provides read access to the block if no password
//! is set or if a password is set and the block is unlocked.  If the block is
//! password protected and locked, neither read nor write access is permitted.
//!
//! \return Returns a logical OR combination of \b EEPROM_RC_INVPL,
//! \b EEPROM_RC_WRBUSY, \b EEPROM_RC_NOPERM, \b EEPROM_RC_WKCOPY,
//! \b EEPROM_RC_WKERASE, and \b EEPROM_RC_WORKING to indicate status and error
//! conditions.
//
//*****************************************************************************
unsigned long
EEPROMBlockProtectSet(unsigned long ulBlock, unsigned long ulProtect)
{
    //
    // Parameter validity check.
    //
    ASSERT(ulBlock < BLOCKS_FROM_EESIZE(HWREG(EEPROM_EESIZE)));

    //
    // Set the current block.
    //
    HWREG(EEPROM_EEBLOCK) = ulBlock;

    //
    // Set the protection options for this block.
    //
    HWREG(EEPROM_EEPROT) = ulProtect;

    //
    // Wait for the write to complete.
    //
    while(HWREG(EEPROM_EEDONE) & EEPROM_EEDONE_WORKING)
    {
        //
        // Still working.
        //
    }

    //
    // Pass any error codes back to the caller.
    //
    return(HWREG(EEPROM_EEDONE));
}

//*****************************************************************************
//
//! Sets the password used to protect an EEPROM block.
//!
//! \param ulBlock is the EEPROM block number for which the password is to be
//! set.
//! \param pulPassword points to an array of unsigned long values comprising
//! the password to set.  Each element may be any 32-bit value other than
//! 0xFFFFFFFF. This array must contain the number of elements given by the
//! \b ulCount parameter.
//! \param ulCount provides the number of unsigned longs in the \b ulPassword.
//! Valid values are 1, 2 and 3.
//!
//! This function allows the password used to unlock an EEPROM block to be
//! set.  Valid passwords may be either 32, 64 or 96 bits comprising words
//! with any value other than 0xFFFFFFFF. The password may only be set once.
//! Any further attempts to set the password result in an error.  Once the
//! password is set, the block remains unlocked until EEPROMBlockLock() is
//! called for that block or block 0, or a reset occurs.
//!
//! If a password is set on block 0, this affects locking of the peripheral as
//! a whole. When block 0 is locked, all other EEPROM blocks are inaccessible
//! until block 0 is unlocked.  Once block 0 is unlocked, other blocks
//! become accessible according to any passwords set on those blocks and the
//! protection set for that block via a call to EEPROMBlockProtectSet().
//!
//! \return Returns a logical OR combination of \b EEPROM_RC_INVPL,
//! \b EEPROM_RC_WRBUSY, \b EEPROM_RC_NOPERM, \b EEPROM_RC_WKCOPY,
//! \b EEPROM_RC_WKERASE, and \b EEPROM_RC_WORKING to indicate status and error
//! conditions.
//
//*****************************************************************************
unsigned long
EEPROMBlockPasswordSet(unsigned long ulBlock, unsigned long *pulPassword,
                       unsigned long ulCount)
{
    unsigned long ulReg;

    //
    // Check parameters in a debug build.
    //
    ASSERT(pulPassword);
    ASSERT(ulBlock < BLOCKS_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
    ASSERT(ulCount <= 3);

    //
    // Set the block number whose password we are about to write.
    //
    HWREG(EEPROM_EEBLOCK) = ulBlock;

    //
    // Start with the first password word.
    //
    ulReg = EEPROM_EEPASS0;

    //
    // Write the password.
    //
    while(ulCount)
    {
        //
        // Start the process of writing the password.
        //
        HWREG(ulReg) = *pulPassword;

        //
        // Update values in preparation for writing the next word.
        //
        pulPassword++;
        ulReg += 4;
        ulCount--;

        //
        // Wait for the last word write to complete or an error to be reported.
        //
        while(HWREG(EEPROM_EEDONE) & EEPROM_EEDONE_WORKING)
        {
            //
            // Still working.
            //
        }
    }

    //
    // Return the final write status.
    //
    return(HWREG(EEPROM_EEDONE));
}

//*****************************************************************************
//
//! Locks a password-protected EEPROM block.
//!
//! \param ulBlock is the EEPROM block number which is to be locked.
//!
//! This function locks an EEPROM block that has previously been protected by
//! writing a password.  Access to the block once it is locked is determined
//! by the protection settings applied via a previous call to the
//! EEPROMBlockProtectSet() function.  If no password has previously been set
//! for the block, this function has no effect.
//!
//! Locking block 0 has the effect of making all other blocks in the EEPROM
//! inaccessible.
//!
//! \return Returns the lock state for the block on exit, 1 if unlocked (as
//! would be the case if no password was set) or 0 if locked.
//!
//*****************************************************************************
unsigned long
EEPROMBlockLock(unsigned long ulBlock)
{
    //
    // Check parameters in a debug build.
    //
    ASSERT(ulBlock < BLOCKS_FROM_EESIZE(HWREG(EEPROM_EESIZE)));

    //
    // Select the block we are going to lock.
    //
    HWREG(EEPROM_EEBLOCK) = ulBlock;

    //
    // Lock the block.
    //
    HWREG(EEPROM_EEUNLOCK) = 0xFFFFFFFF;

    //
    // Return the current lock state.
    //
    return(HWREG(EEPROM_EEUNLOCK));
}

//*****************************************************************************
//
//! Unlocks a password-protected EEPROM block.
//!
//! \param ulBlock is the EEPROM block number which is to be unlocked.
//! \param pulPassword points to an array of unsigned long values containing
//! the password for the blockt.  Each element must match the password
//! originally set via a call to EEPROMBlockPasswordSet().
//! \param ulCount provides the number of unsigned longs in the \b pulPassword
//! array and must match the value originally passed to
//! EEPROMBlockPasswordSet().  Valid values are 1, 2 and 3.
//!
//! This function unlocks an EEPROM block that has previously been protected by
//! writing a password.  Access to the block once it is unlocked is determined
//! by the protection settings applied via a previous call to the
//! EEPROMBlockProtectSet() function.
//!
//! To successfully unlock an EEPROM block, the password provided must match
//! the password provided on the original call to EEPROMBlockPasswordSet().  If
//! an incorrect password is provided, the block remains locked.
//!
//! Unlocking block 0 has the effect of making all other blocks in the device
//! accessible according to their own access protection settings.  When block
//! 0 is locked, all other EEPROM blocks are inaccessible.
//!
//! \return Returns the lock state for the block on exit, 1 if unlocked or 0 if
//! locked.
//!
//*****************************************************************************
unsigned long
EEPROMBlockUnlock(unsigned long ulBlock, unsigned long *pulPassword,
                  unsigned long ulCount)
{
    //
    // Check parameters in a debug build.
    //
    ASSERT(pulPassword);
    ASSERT(ulBlock < BLOCKS_FROM_EESIZE(HWREG(EEPROM_EESIZE)));
    ASSERT(ulCount <= 3);

    //
    // Set the block that we are trying to unlock.
    //
    HWREG(EEPROM_EEBLOCK) = ulBlock;

    //
    // Write the unlock register with 0xFFFFFFFF to reset the unlock
    // sequence just in case a short password was previously used to try to
    // unlock the block.
    //
    HWREG(EEPROM_EEUNLOCK) = 0xFFFFFFFF;

    //
    // We need to write the password words in the opposite order when unlocking
    // compared to locking so start at the end of the array.
    //
    pulPassword += (ulCount - 1);

    //
    // Write the supplied password to unlock the block.
    //
    while(ulCount)
    {
        HWREG(EEPROM_EEUNLOCK) = *pulPassword--;
        ulCount--;
    }

    //
    // Let the caller know if their password worked.
    //
    return(HWREG(EEPROM_EEUNLOCK));
}

//*****************************************************************************
//
//! Hides an EEPROM block until the next reset.
//!
//! \param ulBlock is the EEPROM block number which is to be hidden.
//!
//! This function hides an EEPROM block other than block 0.  Once hidden, a
//! block is completely inaccessible until the next reset.  This mechanism
//! allows initialization code to have access to data which is to be hidden
//! from the rest of the application.  Unlike applications using passwords, an
//! application making using of block hiding need not contain any embedded
//! passwords which could be found through disassembly.
//!
//! \return None.
//!
//*****************************************************************************
void
EEPROMBlockHide(unsigned long ulBlock)
{
    //
    // Check parameters in a debug build.
    //
    ASSERT(!ulBlock);
    ASSERT(ulBlock < BLOCKS_FROM_EESIZE(HWREG(EEPROM_EESIZE)));

    //
    // Hide the requested block.
    //
    HWREG(EEPROM_EEHIDE) = (1 << ulBlock);
}

//*****************************************************************************
//
//! Enables the EEPROM interrupt.
//!
//! \param ulIntFlags indicates which EEPROM interrupt source to enable.  This
//! must be \b EEPROM_INT_PROGRAM currently.
//!
//! This function enables the EEPROM interrupt.  When enabled, an interrupt
//! is generated when any EEPROM write or erase operation completes.  The
//! EEPROM peripheral shares a single interrupt vector with the flash memory
//! subsystem, \b INT_FLASH.  This function is provided as a convenience but
//! the EEPROM interrupt can also be enabled using a call to FlashIntEnable()
//! passing FLASH_INT_EEPROM in the \b ulIntFlags parameter.
//!
//! \return None.
//!
//*****************************************************************************
void
EEPROMIntEnable(unsigned long ulIntFlags)
{
    //
    // Look for valid interrupt sources.
    //
    ASSERT(ulIntFlags == EEPROM_INT_PROGRAM);

    //
    // Enable interrupts from the EEPROM module.
    //
    HWREG(EEPROM_EEINT) |= EEPROM_EEINT_INT;

    //
    // Enable the EEPROM interrupt in the flash controller module.
    //
    HWREG(FLASH_FCIM) |= FLASH_FCRIS_ERIS;
}

//*****************************************************************************
//
//! Disables the EEPROM interrupt.
//!
//! \param ulIntFlags indicates which EEPROM interrupt source to disable.  This
//! must be \b EEPROM_INT_PROGRAM currently.
//!
//! This function disables the EEPROM interrupt and prevents calls to the
//! interrupt vector when any EEPROM write or erase operation completes.  The
//! EEPROM peripheral shares a single interrupt vector with the flash memory
//! subsystem, \e INT_FLASH.    This function is provided as a convenience but
//! the EEPROM interrupt can also be disabled using a call to FlashIntDisable()
//! passing FLASH_INT_EEPROM in the \b ulIntFlags parameter.
//!
//! \return None.
//!
//*****************************************************************************
void
EEPROMIntDisable(unsigned long ulIntFlags)
{
    //
    // Look for valid interrupt sources.
    //
    ASSERT(ulIntFlags == EEPROM_INT_PROGRAM);

    //
    // Disable the EEPROM interrupt in the flash controller module.
    //
    HWREG(FLASH_FCIM) &= ~FLASH_FCIM_EMASK;

    //
    // Disable interrupts from the EEPROM module.
    //
    HWREG(EEPROM_EEINT) &= ~EEPROM_EEINT_INT;
}

//*****************************************************************************
//
//! Reports the state of the EEPROM interrupt.
//!
//! \param bMasked determines whether the masked or unmasked state of the
//! interrupt is to be returned. If bMasked is \e true, the masked state is
//! returned, otherwise the unmasked state is returned.
//!
//! This function allows an application to query the state of the EEPROM
//! interrupt.  If active, the interrupt may be cleared by calling
//! EEPROMIntClear().
//!
//! \return Returns \b EEPROM_INT_PROGRAM if an interrupt is being signaled or
//! 0 otherwise.
//
//*****************************************************************************
unsigned long
EEPROMIntStatus(tBoolean bMasked)
{
    if(bMasked)
    {
        //
        // If asked for the masked interrupt status, we check to see if the
        // relevant interrupt is pending in the flash controller then return
        // the appropriate EEPROM flag if it is.
        //
        return((HWREG(FLASH_FCMISC) & FLASH_FCMISC_EMISC) ?
                EEPROM_INT_PROGRAM : 0);
    }
    else
    {
        //
        // If asked for the unmasked interrupt status, infer that an interrupt
        // is pending if the WORKING bit of the EEDONE register is clear.  The
        // actual interrupt will fire on the high to low transition of this bit
        // but we don't have access to an unmasked interrupt status for the
        // EEPROM since it's handled via the flash controller so we have to
        // make do with this instead.
        //
        return((HWREG(EEPROM_EEDONE) & EEPROM_EEDONE_WORKING) ?
               0 : EEPROM_INT_PROGRAM);
    }
}

//*****************************************************************************
//
//! Clears the EEPROM interrupt.
//!
//! \param ulIntFlags indicates which interrupt sources to clear.  Currently,
//! the only valid value is \b EEPROM_INT_PROGRAM.
//!
//! This function allows an application to clear the EEPROM interrupt.
//!
//! \note Because there is a write buffer in the Cortex-M processor, it may
//! take several clock cycles before the interrupt source is actually cleared.
//! Therefore, it is recommended that the interrupt source be cleared early in
//! the interrupt handler (as opposed to the very last action) to avoid
//! returning from the interrupt handler before the interrupt source is
//! actually cleared.  Failure to do so may result in the interrupt handler
//! being immediately reentered (because the interrupt controller still sees
//! the interrupt source asserted).
//!
//! \return None.
//!
//*****************************************************************************
void
EEPROMIntClear(unsigned long ulIntFlags)
{
    //
    // Clear the flash interrupt.
    //
    HWREG(FLASH_FCMISC) = FLASH_FCMISC_EMISC;

    //
    // Clear the sector protection bits to prevent possible problems when
    // programming the main flash array later.
    //
    if(CLASS_IS_BLIZZARD && REVISION_IS_A0)
    {
        EEPROMClearSectorMask();
    }
}

//*****************************************************************************
//
//! Returns status on the last EEPROM program or erase operation.
//!
//! This function returns the current status of the last program or erase
//! operation performed by the EEPROM.  It is intended to provide error
//! information to applications programming or setting EEPROM protection
//! options under interrupt control.
//!
//! \return Returns 0 if the last program or erase operation completed without
//! any errors.  If an operation is ongoing or an error occurred, the return
//! value is a logical OR combination of \b EEPROM_RC_INVPL,
//! \b EEPROM_RC_WRBUSY, \b EEPROM_RC_NOPERM, \b EEPROM_RC_WKCOPY,
//! \b EEPROM_RC_WKERASE, and \b EEPROM_RC_WORKING.
//!
//*****************************************************************************
unsigned long
EEPROMStatusGet(void)
{
    return(HWREG(EEPROM_EEDONE));
}

//*****************************************************************************
//
// Close the Doxygen group.
//! @}
//
//*****************************************************************************
