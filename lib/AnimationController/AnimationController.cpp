#include <string.h>

#include <AnimationController.h>

AnimationController AnimationCtrl(5);

AnimationController::AnimationController(int l) :
    lowerLimit(l) {
        queueRear = -1;
    }

AnimationController::~AnimationController() {}


bool AnimationController::queueScript(char scriptName[]){
    if (queueIsFull()){
        return false;
    }

    bool loadServoScript = !servoScriptLoaded;

    queueRear = (queueRear + 1) % queueCapacity;

    strncpy(scriptQueue[queueRear], scriptName, commandSize);

    scriptQueue[queueRear][commandSize - 1] = '\0';

    queueSize++;

    if (loadServoScript){
        loadNextScript();
    }

    return true;
}


bool AnimationController::queueIsFull(){
    return (queueSize == queueCapacity); 
}

bool AnimationController::queueIsEmpty(){
    return (queueSize == 0); 
}

void AnimationController::loadNextScript(){

    if (queueIsEmpty()){
        return;
    }

    //LoadFromMemory(scriptQueue[queueFront]);

    /*****************************************
     * Test script
     *****************************************/
    if (strncmp(scriptQueue[queueFront], "#start", strlen("#home")) == 0){
        servoScript[0].setValues(Spinner, Start, 0, 0, 500);
        servoScript[1].setValues(Lifter, Start, 0, 0, 500);
        servoEvents = 2;
    } 
    if (strncmp(scriptQueue[queueFront], "#home", strlen("#home")) == 0){
        servoScript[0].setValues(Spinner, Home, 0, 0, 2000);
        servoScript[1].setValues(Lifter, Home, 0, 0, 2000);
        servoEvents = 2;
    } 
    if (strncmp(scriptQueue[queueFront], "#stow", strlen("#stow")) == 0){
        servoScript[0].setValues(Spinner, Home, 0, 0, 2000);
        servoScript[1].setValues(Lifter, Position, -15, 0, 2000);
        servoEvents = 2;
    } 
    else if (strncmp(scriptQueue[queueFront], "#deploy", strlen("#deploy")) == 0) {
        servoScript[0].setValues(Lifter, Position, 780, 0, 1000);
        servoEvents = 1;
    }
    else if (strncmp(scriptQueue[queueFront], "#sneaky", strlen("#sneaky")) == 0) {
        servoScript[0].setValues(Lifter, Position, 780, 100, 12000);
        servoScript[1].setValues(Lifter, Position, 350, 100, 6000);
        servoScript[2].setValues(Lifter, Position, 690, 100, 4000);
        servoScript[3].setValues(Spinner, Position, -350, 700, 5000);
        servoScript[4].setValues(Spinner, Position, 700, 2000, 3000);
        servoScript[5].setValues(Lifter, Position, 790, 100, 2000);
        servoScript[6].setValues(Spinner, Position, 350, 400, 5000);
        servoScript[7].setValues(Spinner, Home, 0, 0, 1000);
        servoScript[8].setValues(Lifter, Position, -15, 200, 12000);    
        servoEvents = 9;
    }
    /*****************************************
     * End Test Script
     *****************************************/

    queueFront = (queueFront + 1) % queueCapacity;
    queueSize--;
    servoEventsFired = 0;
    servoScriptLoaded = true;
}

bool AnimationController::servoScriptIsLoaded(){
    
    if (!servoScriptLoaded && !queueIsEmpty()){
        loadNextScript();
    }

    return servoScriptLoaded;
}

char* AnimationController::getNextServoCommand(){

    if (!servoScriptLoaded){
        handleLastServoEvent();
        char *str = (char*)malloc(2); 
        snprintf(str, 2, " ");
        return str;
    } else if (servoEventsFired == servoEvents){
        handleLastServoEvent();
        char *str = (char*)malloc(2); 
        snprintf(str, 2, " ");
        return str;
    } else {
        
        //if home lifter, then home spinner first
        //if move lifter to position below threshold, unless home, default to theshold

        int nextEventIndex = servoEventsFired;
        servoEventsFired++;

        if (servoEventsFired == servoEvents){
            handleLastServoEvent();
        }

        delayTillNextServoEvent = servoScript[nextEventIndex].getDuration();
        // 10 milliseconds minimum between events?
        if (delayTillNextServoEvent < 10){
                delayTillNextServoEvent = 10;
        }
        
        return servoScript[nextEventIndex].toCommand();
    }
}

int AnimationController::msTillNextServoCommand(){
    return delayTillNextServoEvent;
}

void setLifterP(int p, bool isMoving){
    //TODO
}

void setSpinnerP(int s, bool isMoving){
    //TODO
}

void AnimationController::handleLastServoEvent(){
    servoScriptLoaded = false;
    servoEvents = 0;
    servoEventsFired = 0;
}

