/*
 * QuadrupedServoControl.cpp
 *
 * Contains all the servo related functions and data.
 *
 * Program for controlling a mePed Robot V2 with 8 servos using an IR Remote at pin A0
 * Supported IR remote are KEYES (the original mePed remote) and WM10
 * Select the one you have at line 23 in QuadrupedIRConfiguration.h
 *
 * To run this example need to install the "ServoEasing", "IRLremote" and "PinChangeInterrupt" libraries under "Tools -> Manage Libraries..." or "Ctrl+Shift+I"
 * Use "ServoEasing", "IRLremote" and "PinChangeInterrupt" as filter string.
 *
 *  Copyright (C) 2019  Armin Joachimsmeyer
 *  armin.joachimsmeyer@gmail.com
 *
 *  This file is part of ServoEasing https://github.com/ArminJo/ServoEasing.
 *
 *  ServoEasing is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/gpl.html>.
 */

#include <Arduino.h>

#include "QuadrupedServoConfiguration.h"
#include "QuadrupedServoControl.h"

#include "IRCommandDispatcher.h" // for checkIRInput(); and RETURN_IF_STOP;

// Define 8 servos in exact this order!
ServoEasing frontLeftPivotServo;    // 0 - Front Left Pivot Servo
ServoEasing frontLeftLiftServo;     // 1 - Front Left Lift Servo
ServoEasing backLeftPivotServo;     // 2 - Back Left Pivot Servo
ServoEasing backLeftLiftServo;      // 3 - Back Left Lift Servo
ServoEasing backRightPivotServo;    // 4 - Back Right Pivot Servo
ServoEasing backRightLiftServo;     // 5 - Back Right Lift Servo
ServoEasing frontRightPivotServo;   // 6 - Front Right Pivot Servo
ServoEasing frontRightLiftServo;    // 7 - Front Right Lift Servo

uint16_t sServoSpeed = 90;      // in degree/second
uint8_t sBodyHeightAngle = LIFT_MIN_ANGLE + 20; // From LIFT_MIN_ANGLE to LIFT_MAX_ANGLE !!! The bigger the angle, the lower the body !!!

// Arrays of trim angles stored in EEPROM
EEMEM int8_t sServoTrimAnglesEEPROM[NUMBER_OF_SERVOS]; // The one which resides in EEPROM and IR read out at startup - filled by eepromWriteServoTrim
int8_t sServoTrimAngles[NUMBER_OF_SERVOS]; // RAM copy for easy setting trim angles by remote, filled by eepromReadServoTrim

void setupQuadrupedServos() {
    // Attach servos to Arduino Pins
    frontLeftPivotServo.attach(5);
    frontLeftLiftServo.attach(6);
    backLeftPivotServo.attach(7);
    backLeftLiftServo.attach(8);
    // Invert direction for lift servos.
    backLeftLiftServo.setReverseOperation(true);
    backRightPivotServo.attach(9);
    backRightLiftServo.attach(10);
    frontRightPivotServo.attach(11);
    frontRightLiftServo.attach(12);
    frontRightLiftServo.setReverseOperation(true);
}

void shutdownServos() {
    Serial.println(F("Shutdown servos"));
    sBodyHeightAngle = LIFT_MAX_ANGLE;
    centerServos();
}

void centerServos() {
    setAllServos(90, 90, 90, 90, sBodyHeightAngle, sBodyHeightAngle, sBodyHeightAngle, sBodyHeightAngle);
}

void printSpeed() {
    Serial.print(F(" Speed="));
    Serial.println(sServoSpeed);
}

void printTrimAngles() {
    for (uint8_t i = 0; i < NUMBER_OF_SERVOS; ++i) {
        Serial.print(F("ServoTrimAngle["));
        Serial.print(i);
        Serial.print(F("]="));
        Serial.println(sServoTrimAngles[i]);
        sServoArray[i]->setTrim(sServoTrimAngles[i]);
    }
}

void resetServosTo90Degree() {
    for (uint8_t i = 0; i < NUMBER_OF_SERVOS; ++i) {
        sServoArray[i]->write(90);
    }
}

/*
 * Copy calibration array from EEPROM to RAM and set uninitialized values to 0
 */
void eepromReadAndSetServoTrim() {
    Serial.println(F("eepromReadAndSetServoTrim()"));
    eeprom_read_block((void*) &sServoTrimAngles, &sServoTrimAnglesEEPROM, NUMBER_OF_SERVOS);
    printTrimAngles();
}

void eepromWriteServoTrim() {
    eeprom_write_block((void*) &sServoTrimAngles, &sServoTrimAnglesEEPROM, NUMBER_OF_SERVOS);
    printTrimAngles();
}

void setEasingTypeToLinear() {
    for (uint8_t tServoIndex = 0; tServoIndex < NUMBER_OF_SERVOS; ++tServoIndex) {
        sServoArray[tServoIndex]->setEasingType(EASE_LINEAR);
    }
}

void setEasingTypeForMoving() {
    for (int tServoIndex = 0; tServoIndex < NUMBER_OF_SERVOS; ++tServoIndex) {
        sServoArray[tServoIndex]->setEasingType(EASE_LINEAR);
        tServoIndex++;
        sServoArray[tServoIndex]->setEasingType(EASE_QUADRATIC_BOUNCING);
    }
}

/*
 * Main transformation routines
 *
 * Direction forward changes nothing.
 * Direction backward swaps forward and backward servos / increases index by NUMBER_OF_LEGS/2
 * Direction left increases index by 1 and right by 3.
 * Mirroring swaps left and right (XOR with 0x06) and invert all angles.
 */

uint8_t getMirrorXorMask(uint8_t aDirection) {
// XOR the index with this value to get the mirrored index
    if (aDirection & MOVE_DIRECTION_SIDE_MASK) {
        return 0x2;
    } else {
        return 0x6;
    }
}

void transformAndSetAllServos(int aFLP, int aBLP, int aBRP, int aFRP, int aFLL, int aBLL, int aBRL, int aFRL, uint8_t aDirection,
        bool doMirror, bool aDoMove) {
    uint8_t tIndexToAdd = aDirection * SERVOS_PER_LEG;
    uint8_t tXorToGetMirroredIndex = 0x0;
// Invert angles for pivot servos
    bool doInvert = false;
    if (doMirror) {
// XOR the index with this value to get the mirrored index
        tXorToGetMirroredIndex = getMirrorXorMask(aDirection);
        doInvert = true;
    }

    uint8_t tEffectivePivotServoIndex;
    tEffectivePivotServoIndex = ((FRONT_LEFT_PIVOT + tIndexToAdd) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
    if (doInvert) {
        aFLP = 180 - aFLP;
    }
    sServoNextPositionArray[tEffectivePivotServoIndex] = aFLP;
    sServoNextPositionArray[tEffectivePivotServoIndex + LIFT_SERVO_OFFSET] = aFLL;

    tEffectivePivotServoIndex = ((BACK_LEFT_PIVOT + tIndexToAdd) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
    if (doInvert) {
        aBLP = 180 - aBLP;
    }
    sServoNextPositionArray[tEffectivePivotServoIndex] = aBLP;
    sServoNextPositionArray[tEffectivePivotServoIndex + LIFT_SERVO_OFFSET] = aBLL;

    tEffectivePivotServoIndex = ((BACK_RIGHT_PIVOT + tIndexToAdd) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
    if (doInvert) {
        aBRP = 180 - aBRP;
    }
    sServoNextPositionArray[tEffectivePivotServoIndex] = aBRP;
    sServoNextPositionArray[tEffectivePivotServoIndex + LIFT_SERVO_OFFSET] = aBRL;

    tEffectivePivotServoIndex = ((FRONT_RIGHT_PIVOT + tIndexToAdd) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
    if (doInvert) {
        aFRP = 180 - aFRP;
    }
    sServoNextPositionArray[tEffectivePivotServoIndex] = aFRP;
    sServoNextPositionArray[tEffectivePivotServoIndex + LIFT_SERVO_OFFSET] = aFRL;

    if (aDoMove) {
        synchronizeMoveAllServosAndCheckInputAndWait();
    }
}

/*
 * A subset of the functionality of transformAndSetAllServos() -> less arguments needed :-)
 */
void transformAndSetPivotServos(int aFLP, int aBLP, int aBRP, int aFRP, uint8_t aDirection, bool doMirror, bool aDoMove) {
    uint8_t tIndexToAdd = aDirection * SERVOS_PER_LEG;
    uint8_t tXorToGetMirroredIndex = 0x0;
// Invert angles for pivot servos
    bool doInvert = false;
    if (doMirror) {
// XOR the index with this value to get the mirrored index
        tXorToGetMirroredIndex = getMirrorXorMask(aDirection);
        doInvert = true;
    }

    uint8_t tEffectivePivotServoIndex;
    tEffectivePivotServoIndex = ((FRONT_LEFT_PIVOT + tIndexToAdd) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
    if (doInvert) {
        aFLP = 180 - aFLP;
    }
    sServoNextPositionArray[tEffectivePivotServoIndex] = aFLP;

    tEffectivePivotServoIndex = ((BACK_LEFT_PIVOT + tIndexToAdd) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
    if (doInvert) {
        aBLP = 180 - aBLP;
    }
    sServoNextPositionArray[tEffectivePivotServoIndex] = aBLP;

    tEffectivePivotServoIndex = ((BACK_RIGHT_PIVOT + tIndexToAdd) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
    if (doInvert) {
        aBRP = 180 - aBRP;
    }
    sServoNextPositionArray[tEffectivePivotServoIndex] = aBRP;

    tEffectivePivotServoIndex = ((FRONT_RIGHT_PIVOT + tIndexToAdd) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
    if (doInvert) {
        aFRP = 180 - aFRP;
    }
    sServoNextPositionArray[tEffectivePivotServoIndex] = aFRP;

    if (aDoMove) {
        synchronizeMoveAllServosAndCheckInputAndWait();
    }
}

/*
 * Transform index of servo by direction and mirroring
 */
uint8_t transformOneServoIndex(uint8_t aServoIndexToTransform, uint8_t aDirection, bool doMirror) {
    uint8_t tXorToGetMirroredIndex = 0x0;
    if (doMirror) {
// XOR the index with this value to get the mirrored index
        tXorToGetMirroredIndex = getMirrorXorMask(aDirection);
    }
    return ((aServoIndexToTransform + (aDirection * SERVOS_PER_LEG)) % NUMBER_OF_SERVOS) ^ tXorToGetMirroredIndex;
}

void testTransform() {
// left legs are close together, right legs are in straight right direction
    transformAndSetAllServos(180, 1, 135, 30, 111, 0, 0, 0, MOVE_DIRECTION_FORWARD, false, false);
    printArrayPositions(&Serial);
    transformAndSetAllServos(180, 1, 135, 30, 111, 0, 0, 0, MOVE_DIRECTION_FORWARD, true, false);
    printArrayPositions(&Serial);
    transformAndSetAllServos(180, 1, 135, 30, 111, 0, 0, 0, MOVE_DIRECTION_BACKWARD, false, false);
    printArrayPositions(&Serial);
    transformAndSetAllServos(180, 1, 135, 30, 111, 0, 0, 0, MOVE_DIRECTION_BACKWARD, true, false);
    printArrayPositions(&Serial);
    transformAndSetAllServos(180, 1, 135, 30, 111, 0, 0, 0, MOVE_DIRECTION_LEFT, false, false);
    printArrayPositions(&Serial);
    transformAndSetAllServos(180, 1, 135, 30, 111, 0, 0, 0, MOVE_DIRECTION_LEFT, true, false);
    printArrayPositions(&Serial);
}

void setPivotServos(int aFLP, int aBLP, int aBRP, int aFRP) {
    sServoNextPositionArray[FRONT_LEFT_PIVOT] = aFLP;
    sServoNextPositionArray[BACK_LEFT_PIVOT] = aBLP;
    sServoNextPositionArray[BACK_RIGHT_PIVOT] = aBRP;
    sServoNextPositionArray[FRONT_RIGHT_PIVOT] = aFRP;
    synchronizeMoveAllServosAndCheckInputAndWait();
}

/*
 * Accepts height from 0 to 100
 */
void setLiftServoHeight(ServoEasing & aLiftServo, uint8_t aHeightPercent) {
    if (aHeightPercent > 100) {
        aHeightPercent = 100;
    }
    int tDegreeForLiftServo = map(aHeightPercent, 0, 100, LIFT_MAX_ANGLE, LIFT_MIN_ANGLE);
    aLiftServo.easeTo(tDegreeForLiftServo);
}

/*
 * Set all servos to the same angle
 */
void setLiftServos(int aBodyHeightAngle) {
    sServoNextPositionArray[FRONT_LEFT_LIFT] = aBodyHeightAngle;
    sServoNextPositionArray[BACK_LEFT_LIFT] = aBodyHeightAngle;
    sServoNextPositionArray[BACK_RIGHT_LIFT] = aBodyHeightAngle;
    sServoNextPositionArray[FRONT_RIGHT_LIFT] = aBodyHeightAngle;
    synchronizeMoveAllServosAndCheckInputAndWait();
}

void setLiftServos(int aFLL, int aBLL, int aBRL, int aFRL) {
    sServoNextPositionArray[FRONT_LEFT_LIFT] = aFLL;
    sServoNextPositionArray[BACK_LEFT_LIFT] = aBLL;
    sServoNextPositionArray[BACK_RIGHT_LIFT] = aBRL;
    sServoNextPositionArray[FRONT_RIGHT_LIFT] = aFRL;
    synchronizeMoveAllServosAndCheckInputAndWait();
}

/*
 * Used after change of sBodyHeightAngle
 */
void setLiftServosToBodyHeight() {
    // Set values direct, since we expect only a change of 2 degree
    for (uint8_t i = LIFT_SERVO_OFFSET; i < NUMBER_OF_SERVOS; i += SERVOS_PER_LEG) {
        sServoArray[i]->write(sBodyHeightAngle);
    }
}

void setAllServos(int aFLP, int aBLP, int aBRP, int aFRP, int aFLL, int aBLL, int aBRL, int aFRL) {
    sServoNextPositionArray[FRONT_LEFT_PIVOT] = aFLP;
    sServoNextPositionArray[BACK_LEFT_PIVOT] = aBLP;
    sServoNextPositionArray[BACK_RIGHT_PIVOT] = aBRP;
    sServoNextPositionArray[FRONT_RIGHT_PIVOT] = aFRP;

    sServoNextPositionArray[FRONT_LEFT_LIFT] = aFLL;
    sServoNextPositionArray[BACK_LEFT_LIFT] = aBLL;
    sServoNextPositionArray[BACK_RIGHT_LIFT] = aBRL;
    sServoNextPositionArray[FRONT_RIGHT_LIFT] = aFRL;
    synchronizeMoveAllServosAndCheckInputAndWait();
}

void moveOneServoAndCheckInputAndWait(uint8_t aServoIndex, int aDegree) {
    moveOneServoAndCheckInputAndWait(aServoIndex, aDegree, sServoSpeed);
}

void moveOneServoAndCheckInputAndWait(uint8_t aServoIndex, int aDegree, uint16_t aDegreesPerSecond) {
    sServoArray[aServoIndex]->startEaseTo(aDegree, aDegreesPerSecond, false);
    do {
        checkIRInput();
        RETURN_IF_STOP;
        delay(REFRESH_INTERVAL / 1000); // 20ms - REFRESH_INTERVAL is in Microseconds
    } while (!sServoArray[aServoIndex]->update());
}

void updateAndCheckInputAndWaitForAllServosToStop() {
    do {
        checkIRInput();
        RETURN_IF_STOP;
        delay(REFRESH_INTERVAL / 1000); // 20ms - REFRESH_INTERVAL is in Microseconds
    } while (!updateAllServos());
}

void synchronizeMoveAllServosAndCheckInputAndWait() {
    setEaseToForAllServos();
    synchronizeAllServosAndStartInterrupt(false);
    updateAndCheckInputAndWaitForAllServosToStop();
}