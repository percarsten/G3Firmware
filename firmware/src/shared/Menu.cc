#include "Menu.hh"
#include "Configuration.hh"

// TODO: Kill this, should be hanlded by build system.
#ifdef HAS_INTERFACE_BOARD

#include "Steppers.hh"
#include "Commands.hh"
#include "Errors.hh"
#include "Tool.hh"
#include "Host.hh"
#include "Timeout.hh"
#include "InterfaceBoard.hh"
#include "Interface.hh"
#include "Motherboard.hh"
#include "Version.hh"
#include <util/delay.h>
#include <stdlib.h>
#include "SDCard.hh"
#include "EepromMap.hh"
#include "Eeprom.hh"
#include <avr/eeprom.h>


#define HOST_PACKET_TIMEOUT_MS 20
#define HOST_PACKET_TIMEOUT_MICROS (1000L*HOST_PACKET_TIMEOUT_MS)

#define HOST_TOOL_RESPONSE_TIMEOUT_MS 50
#define HOST_TOOL_RESPONSE_TIMEOUT_MICROS (1000L*HOST_TOOL_RESPONSE_TIMEOUT_MS)

/// Send a packet to the extruder.  If cmdType == EXTDR_CMD_SET, then "val" should
/// contain the value to be written otherwise val is ignored.
/// responsePacket is filled with the returned value
bool extruderControl(uint8_t command, enum extruderCommandType cmdType,
		     OutPacket& responsePacket, uint16_t val) {

	Timeout acquire_lock_timeout;
	acquire_lock_timeout.start(HOST_TOOL_RESPONSE_TIMEOUT_MS);
	while (!tool::getLock()) {
		if (acquire_lock_timeout.hasElapsed()) {
			return false;
		}
	}
	OutPacket& out = tool::getOutPacket();
	InPacket& in = tool::getInPacket();
	out.reset();
	responsePacket.reset();

	// Fill the query packet. The first byte is the toolhead index, and the
	// second is the
	out.append8(0);
	out.append8(command);
	if ( cmdType == EXTDR_CMD_SET )	out.append16(val);	

	// Timeouts are handled inside the toolslice code; there's no need
	// to check for timeouts on this loop.
	tool::startTransaction();
	tool::releaseLock();
	// WHILE: bounded by tool timeout in runToolSlice
	while (!tool::isTransactionDone()) {
		tool::runToolSlice();
	}
	if (in.getErrorCode() == PacketError::PACKET_TIMEOUT) {
		return false;
	} else {
		// Copy payload back. Start from 0-- we need the response code.
		for (uint8_t i = 0; i < in.getLength(); i++) {
			responsePacket.append8(in.read8(i));
		}
	}

	// Check that the extruder was able to process the request
	if (!rcCompare(responsePacket.read8(0),RC_OK)) {
		return false;
	}

	return true;
}



void strcat(char *buf, const char* str)
{
	char *ptr = buf;
	while (*ptr) ptr++;
	while (*str) *ptr++ = *str++;
	*ptr++ = '\0';
}


int appendTime(char *buf, uint8_t buflen, uint32_t val)
{
	bool hasdigit = false;
	uint8_t idx = 0;
	uint8_t written = 0;

	if (buflen < 1) {
		return written;
	}

	while (idx < buflen && buf[idx]) idx++;
	if (idx >= buflen-1) {
		buf[buflen-1] = '\0';
		return written;
	}

	uint8_t radidx = 0;
	const uint8_t radixcount = 5;
	const uint8_t houridx = 2;
	const uint8_t minuteidx = 4;
	uint32_t radixes[radixcount] = {360000, 36000, 3600, 600, 60};
	if (val >= 3600000) {
		val %= 3600000;
	}
	for (radidx = 0; radidx < radixcount; radidx++) {
		char digit = '0';
		uint8_t bit = 8;
		uint32_t radshift = radixes[radidx] << 3;
		for (; bit > 0; bit >>= 1, radshift >>= 1) {
			if (val > radshift) {
				val -= radshift;
				digit += bit;
			}
		}
		if (hasdigit || digit != '0' || radidx >= houridx) {
			buf[idx++] = digit;
			hasdigit = true;
		} else {
			buf[idx++] = ' ';
		}
		if (idx >= buflen) {
			buf[buflen-1] = '\0';
			return written;
		}
		written++;
		if (radidx == houridx) {
			buf[idx++] = 'h';
			if (idx >= buflen) {
				buf[buflen-1] = '\0';
				return written;
			}
			written++;
		}
		if (radidx == minuteidx) {
			buf[idx++] = 'm';
			if (idx >= buflen) {
				buf[buflen-1] = '\0';
				return written;
			}
			written++;
		}
	}

	if (idx < buflen) {
		buf[idx] = '\0';
	} else {
		buf[buflen-1] = '\0';
	}

	return written;
}



int appendUint8(char *buf, uint8_t buflen, uint8_t val)
{
	bool hasdigit = false;
	uint8_t written = 0;
	uint8_t idx = 0;

	if (buflen < 1) {
		return written;
	}

	while (idx < buflen && buf[idx]) idx++;
	if (idx >= buflen-1) {
		buf[buflen-1] = '\0';
		return written;
	}

	if (val >= 100) {
		uint8_t res = val / 100;
		val -= res * 100;
		buf[idx++] = '0' + res;
		if (idx >= buflen) {
			buf[buflen-1] = '\0';
			return written;
		}
		hasdigit = true;
		written++;
	} else {
		buf[idx++] = ' ';
		if (idx >= buflen) {
			buf[buflen-1] = '\0';
			return written;
		}
		written++;
	}

	if (val >= 10 || hasdigit) {
		uint8_t res = val / 10;
		val -= res * 10;
		buf[idx++] = '0' + res;
		if (idx >= buflen) {
			buf[buflen-1] = '\0';
			return written;
		}
		hasdigit = true;
		written++;
	} else {
		buf[idx++] = ' ';
		if (idx >= buflen) {
			buf[buflen-1] = '\0';
			return written;
		}
		written++;
	}

	buf[idx++] = '0' + val;
	if (idx >= buflen) {
		buf[buflen-1] = '\0';
		return written;
	}
	written++;

	if (idx < buflen) {
		buf[idx] = '\0';
	} else {
		buf[buflen-1] = '\0';
	}

	return written;
}



void SplashScreen::update(LiquidCrystal& lcd, bool forceRedraw) {
	static PROGMEM prog_uchar splash1[] = "                ";
	static PROGMEM prog_uchar splash2[] = " Thing-O-Matic  ";
	static PROGMEM prog_uchar splash3[] = "   ---------    ";
	static PROGMEM prog_uchar splash4[] = "                ";


	if (forceRedraw) {
		lcd.setCursor(0,0);
		lcd.writeFromPgmspace(splash1);

		lcd.setCursor(0,1);
		lcd.writeFromPgmspace(splash2);

		lcd.setCursor(0,2);
		lcd.writeFromPgmspace(splash3);

		lcd.setCursor(0,3);
		lcd.writeFromPgmspace(splash4);
	}
	else {
		// The machine has started, so we're done!
                interface::popScreen();
        }
}

void SplashScreen::notifyButtonPressed(ButtonArray::ButtonName button) {
	// We can't really do anything, since the machine is still loading, so ignore.
}

void SplashScreen::reset() {
}

void JogMode::reset() {
	jogDistance = DISTANCE_SHORT;
	distanceChanged = false;
	lastDirectionButtonPressed = (ButtonArray::ButtonName)0;
}

void JogMode::update(LiquidCrystal& lcd, bool forceRedraw) {
	static PROGMEM prog_uchar jog1[] = "Jog mode: ";
	static PROGMEM prog_uchar jog2[] = "  Y+          Z+";
	static PROGMEM prog_uchar jog3[] = "X-  X+    (mode)";
	static PROGMEM prog_uchar jog4[] = "  Y-          Z-";

	static PROGMEM prog_uchar distanceShort[] = "SHORT";
	static PROGMEM prog_uchar distanceLong[] = "LONG";
	static PROGMEM prog_uchar distanceCont[] = "CONT";

	if (forceRedraw || distanceChanged) {
		lcd.clear();
		lcd.setCursor(0,0);
		lcd.writeFromPgmspace(jog1);

		switch (jogDistance) {
		case DISTANCE_SHORT:
			lcd.writeFromPgmspace(distanceShort);
			break;
		case DISTANCE_LONG:
			lcd.writeFromPgmspace(distanceLong);
			break;
		case DISTANCE_CONT:
			lcd.writeFromPgmspace(distanceCont);
			break;
		}

		lcd.setCursor(0,1);
		lcd.writeFromPgmspace(jog2);

		lcd.setCursor(0,2);
		lcd.writeFromPgmspace(jog3);

		lcd.setCursor(0,3);
		lcd.writeFromPgmspace(jog4);

		distanceChanged = false;
	}

	if ( jogDistance == DISTANCE_CONT ) {
		if ( lastDirectionButtonPressed ) {
			if (interface::isButtonPressed(lastDirectionButtonPressed))
				JogMode::notifyButtonPressed(lastDirectionButtonPressed);
			else	lastDirectionButtonPressed = (ButtonArray::ButtonName)0;
		}
	}
}

void JogMode::jog(ButtonArray::ButtonName direction) {
	Point position = steppers::getPosition();

	int32_t interval = 2000;
	uint8_t steps;

	if ( jogDistance == DISTANCE_CONT )	interval = 1000;

	switch(jogDistance) {
	case DISTANCE_SHORT:
		steps = 20;
		break;
	case DISTANCE_LONG:
		steps = 200;
		break;
	case DISTANCE_CONT:
		steps = 50;
		break;
	}

	switch(direction) {
        case ButtonArray::XMINUS:
		position[0] -= steps;
		break;
        case ButtonArray::XPLUS:
		position[0] += steps;
		break;
        case ButtonArray::YMINUS:
		position[1] -= steps;
		break;
        case ButtonArray::YPLUS:
		position[1] += steps;
		break;
        case ButtonArray::ZMINUS:
		position[2] -= steps;
		break;
        case ButtonArray::ZPLUS:
		position[2] += steps;
		break;
	}

	if ( jogDistance == DISTANCE_CONT )	lastDirectionButtonPressed = direction;
	else					lastDirectionButtonPressed = (ButtonArray::ButtonName)0;

	steppers::setTarget(position, interval);
}

void JogMode::notifyButtonPressed(ButtonArray::ButtonName button) {
	switch (button) {
        case ButtonArray::ZERO:
        case ButtonArray::OK:
		switch(jogDistance)
		{
			case DISTANCE_SHORT:
				jogDistance = DISTANCE_LONG;
				break;
			case DISTANCE_LONG:
				jogDistance = DISTANCE_CONT;
				break;
			case DISTANCE_CONT:
				jogDistance = DISTANCE_SHORT;
				break;
		}
		distanceChanged = true;
		break;
        case ButtonArray::YMINUS:
        case ButtonArray::ZMINUS:
        case ButtonArray::YPLUS:
        case ButtonArray::ZPLUS:
        case ButtonArray::XMINUS:
        case ButtonArray::XPLUS:
		jog(button);
		break;
        case ButtonArray::CANCEL:
                interface::popScreen();
		break;
	}
}


void SnakeMode::update(LiquidCrystal& lcd, bool forceRedraw) {
	static PROGMEM prog_uchar gameOver[] =  "GAME OVER!";

	// If we are dead, restart the game.
	if (!snakeAlive) {
		reset();
		forceRedraw = true;
	}

	if (forceRedraw) {
		lcd.clear();

		for (uint8_t i = 0; i < snakeLength; i++) {
			lcd.setCursor(snakeBody[i].x, snakeBody[i].y);
			lcd.write('O');
		}
	}

	// Always redraw the apple, just in case.
	lcd.setCursor(applePosition.x, applePosition.y);
	lcd.write('*');

	// First, undraw the snake's tail
	lcd.setCursor(snakeBody[snakeLength-1].x, snakeBody[snakeLength-1].y);
	lcd.write(' ');

	// Then, shift the snakes body parts back, deleting the tail
	for(int8_t i = snakeLength-1; i >= 0; i--) {
		snakeBody[i+1] = snakeBody[i];
	}

	// Create a new head for the snake (this causes it to move forward)
	switch(snakeDirection)
	{
	case DIR_EAST:
		snakeBody[0].x = (snakeBody[0].x + 1) % LCD_SCREEN_WIDTH;
		break;
	case DIR_WEST:
		snakeBody[0].x = (snakeBody[0].x +  LCD_SCREEN_WIDTH - 1) % LCD_SCREEN_WIDTH;
		break;
	case DIR_NORTH:
		snakeBody[0].y = (snakeBody[0].y + LCD_SCREEN_HEIGHT - 1) % LCD_SCREEN_HEIGHT;
		break;
	case DIR_SOUTH:
		snakeBody[0].y = (snakeBody[0].y + 1) % LCD_SCREEN_HEIGHT;
		break;
	}

	// Now, draw the snakes new head
	lcd.setCursor(snakeBody[0].x, snakeBody[0].y);
	lcd.write('O');

	// Check if the snake has run into itself
	for (uint8_t i = 1; i < snakeLength; i++) {
		if (snakeBody[i].x == snakeBody[0].x
			&& snakeBody[i].y == snakeBody[0].y) {
			snakeAlive = false;

			lcd.setCursor(1,1);
			lcd.writeFromPgmspace(gameOver);
			updateRate = 5000L * 1000L;
		}
	}

	// If the snake just ate an apple, increment count and make new apple
	if (snakeBody[0].x == applePosition.x
			&& snakeBody[0].y == applePosition.y) {
		applesEaten++;

		if(applesEaten % APPLES_BEFORE_GROW == 0) {
			snakeLength++;
			updateRate -= 5L * 1000L;
		}

		applePosition.x = rand()%LCD_SCREEN_WIDTH;
		applePosition.y = rand()%LCD_SCREEN_HEIGHT;

		lcd.setCursor(applePosition.x, applePosition.y);
		lcd.write('*');
	}
}

void SnakeMode::reset() {
	updateRate = 150L * 1000L;
	snakeDirection = DIR_EAST;
	snakeLength = 3;
	applesEaten = 0;
	snakeAlive = true;

	// Put the snake in an initial position
	snakeBody[0].x = 2; snakeBody[0].y = 1;
	snakeBody[1].x = 1; snakeBody[1].y = 1;
	snakeBody[2].x = 0; snakeBody[2].y = 1;

	// Put the apple in an initial position (this could collide with the snake!)
	applePosition.x = rand()%LCD_SCREEN_WIDTH;
	applePosition.y = rand()%LCD_SCREEN_HEIGHT;
}


void SnakeMode::notifyButtonPressed(ButtonArray::ButtonName button) {
	switch (button) {
        case ButtonArray::YMINUS:
		snakeDirection = DIR_SOUTH;
		break;
        case ButtonArray::YPLUS:
		snakeDirection = DIR_NORTH;
		break;
        case ButtonArray::XMINUS:
		snakeDirection = DIR_WEST;
		break;
        case ButtonArray::XPLUS:
		snakeDirection = DIR_EAST;
		break;
        case ButtonArray::CANCEL:
                interface::popScreen();
		break;
	}
}


void MonitorMode::reset() {
	updatePhase = 0;
	buildTimePhase = 0;
	buildComplete = false;
	extruderStartSeconds = 0.0;
	lastElapsedSeconds = 0.0;
}

void MonitorMode::update(LiquidCrystal& lcd, bool forceRedraw) {
	static PROGMEM prog_uchar extruder_temp[]      =   "Tool: ---/---C";
	static PROGMEM prog_uchar platform_temp[]      =   "Bed:  ---/---C";
	static PROGMEM prog_uchar elapsed_time[]       =   "Elapsed:   0h00m";
	static PROGMEM prog_uchar completed_percent[]  =   "Completed:   0% ";
	static PROGMEM prog_uchar time_left[]          =   "TimeLeft:  0h00m";
	static PROGMEM prog_uchar time_left_calc[]     =   " calc..";
	static PROGMEM prog_uchar time_left_1min[]     =   "  <1min";
	static PROGMEM prog_uchar time_left_none[]     =   "   none";
	char buf[17];

	if (forceRedraw) {
		lcd.clear();
		lcd.setCursor(0,0);
		switch(host::getHostState()) {
		case host::HOST_STATE_READY:
			lcd.writeString(host::getMachineName());
			break;
		case host::HOST_STATE_BUILDING:
		case host::HOST_STATE_BUILDING_FROM_SD:
			lcd.writeString(host::getBuildName());
			lcd.setCursor(0,1);
			lcd.writeFromPgmspace(completed_percent);
			break;
		case host::HOST_STATE_ERROR:
			lcd.writeString("error!");
			break;
		}

		lcd.setCursor(0,2);
		lcd.writeFromPgmspace(extruder_temp);

		lcd.setCursor(0,3);
		lcd.writeFromPgmspace(platform_temp);
	}


	OutPacket responsePacket;

	// Redraw tool info
	switch (updatePhase) {
	case 0:
		lcd.setCursor(6,2);
		if (extruderControl(SLAVE_CMD_GET_TEMP, EXTDR_CMD_GET, responsePacket, 0)) {
			uint16_t data = responsePacket.read16(1);
			lcd.writeInt(data,3);
		} else {
			lcd.writeString("XXX");
		}
		break;

	case 1:
		lcd.setCursor(10,2);
		if (extruderControl(SLAVE_CMD_GET_SP, EXTDR_CMD_GET, responsePacket, 0)) {
			uint16_t data = responsePacket.read16(1);
			lcd.writeInt(data,3);
		} else {
			lcd.writeString("XXX");
		}
		break;

	case 2:
		lcd.setCursor(6,3);
		if (extruderControl(SLAVE_CMD_GET_PLATFORM_TEMP, EXTDR_CMD_GET, responsePacket, 0)) {
			uint16_t data = responsePacket.read16(1);
			lcd.writeInt(data,3);
		} else {
			lcd.writeString("XXX");
		}
		break;

	case 3:
		lcd.setCursor(10,3);
		if (extruderControl(SLAVE_CMD_GET_PLATFORM_SP, EXTDR_CMD_GET, responsePacket, 0)) {
			uint16_t data = responsePacket.read16(1);
			lcd.writeInt(data,3);
		} else {
			lcd.writeString("XXX");
		}
		break;
	case 4:
		enum host::HostState hostState = host::getHostState();
		
		if ( (hostState != host::HOST_STATE_BUILDING ) && ( hostState != host::HOST_STATE_BUILDING_FROM_SD )) break;

		float secs;

		switch (buildTimePhase) {
			case 0:
				lcd.setCursor(0,1);
				lcd.writeFromPgmspace(completed_percent);
				lcd.setCursor(11,1);
				buf[0] = '\0';
				appendUint8(buf, sizeof(buf), (uint8_t)sdcard::getPercentPlayed());
				strcat(buf, "% ");
				lcd.writeString(buf);
				break;
			case 1:
				lcd.setCursor(0,1);
				lcd.writeFromPgmspace(elapsed_time);
				lcd.setCursor(9,1);
				buf[0] = '\0';

				if ( host::isBuildComplete() ) secs = lastElapsedSeconds; //We stop counting elapsed seconds when we are done
				else {
					lastElapsedSeconds = Motherboard::getBoard().getCurrentSeconds();
					secs = lastElapsedSeconds;
				}
				appendTime(buf, sizeof(buf), (uint32_t)secs);
				lcd.writeString(buf);
				break;
			case 2:
				lcd.setCursor(0,1);
				lcd.writeFromPgmspace(time_left);
				lcd.setCursor(9,1);

				if (( sdcard::getPercentPlayed() >= 1.0 ) && ( extruderStartSeconds > 0.0)) {
					buf[0] = '\0';
					float currentSeconds = Motherboard::getBoard().getCurrentSeconds() - extruderStartSeconds;
					secs = ((currentSeconds / sdcard::getPercentPlayed()) * 100.0 ) - currentSeconds;

					if ((secs > 0.0 ) && (secs < 60.0) && ( ! buildComplete ) )
						lcd.writeFromPgmspace(time_left_1min);	
					else if (( secs <= 0.0) || ( host::isBuildComplete() ) || ( buildComplete ) ) {
						buildComplete = true;
						lcd.writeFromPgmspace(time_left_none);
					} else {
						appendTime(buf, sizeof(buf), (uint32_t)secs);
						lcd.writeString(buf);
					}
				}
				else	lcd.writeFromPgmspace(time_left_calc);

				//Set extruderStartSeconds to when the extruder starts extruding, so we can 
				//get an accurate TimeLeft:
				if ( extruderStartSeconds == 0.0 ) {
					if (extruderControl(SLAVE_CMD_GET_MOTOR_1_PWM, EXTDR_CMD_GET, responsePacket, 0)) {
						uint8_t pwm = responsePacket.read8(1);
						if ( pwm ) extruderStartSeconds = Motherboard::getBoard().getCurrentSeconds();
					}
				}
				break;
		}

		buildTimePhase ++;
		if ( buildTimePhase >= 3 )	buildTimePhase = 0;
		break;
	}

	updatePhase++;
	if (updatePhase > 4) {
		updatePhase = 0;
	}
}

void MonitorMode::notifyButtonPressed(ButtonArray::ButtonName button) {
	switch (button) {
        case ButtonArray::CANCEL:
		switch(host::getHostState()) {
		case host::HOST_STATE_BUILDING:
		case host::HOST_STATE_BUILDING_FROM_SD:
                        interface::pushScreen(&cancelBuildMenu);
			break;
		default:
                        interface::popScreen();
			break;
		}
	}
}

void VersionMode::reset() {
}

void VersionMode::update(LiquidCrystal& lcd, bool forceRedraw) {
	static PROGMEM prog_uchar version1[] = "Firmware Version";
	static PROGMEM prog_uchar version2[] = "----------------";
	static PROGMEM prog_uchar version3[] = "Motherboard: _._";
	static PROGMEM prog_uchar version4[] = "   Extruder: _._";

	if (forceRedraw) {
		lcd.clear();

		lcd.setCursor(0,0);
		lcd.writeFromPgmspace(version1);

		lcd.setCursor(0,1);
		lcd.writeFromPgmspace(version2);

		lcd.setCursor(0,2);
		lcd.writeFromPgmspace(version3);

		lcd.setCursor(0,3);
		lcd.writeFromPgmspace(version4);

		//Display the motherboard version
		lcd.setCursor(13, 2);
		lcd.writeInt(firmware_version / 100, 1);

		lcd.setCursor(15, 2);
		lcd.writeInt(firmware_version % 100, 1);

		//Display the extruder version
		OutPacket responsePacket;

		if (extruderControl(SLAVE_CMD_VERSION, EXTDR_CMD_GET, responsePacket, 0)) {
			uint16_t extruderVersion = responsePacket.read16(1);

			lcd.setCursor(13, 3);
			lcd.writeInt(extruderVersion / 100, 1);

			lcd.setCursor(15, 3);
			lcd.writeInt(extruderVersion % 100, 1);
		} else {
			lcd.setCursor(13, 3);
			lcd.writeString("X.X");
		}
	} else {
	}
}

void VersionMode::notifyButtonPressed(ButtonArray::ButtonName button) {
	interface::popScreen();
}

void Menu::update(LiquidCrystal& lcd, bool forceRedraw) {
	static PROGMEM prog_uchar blankLine[] =  "                ";

	// Do we need to redraw the whole menu?
	if ((itemIndex/LCD_SCREEN_HEIGHT) != (lastDrawIndex/LCD_SCREEN_HEIGHT)
			|| forceRedraw ) {
		// Redraw the whole menu
		lcd.clear();

		for (uint8_t i = 0; i < LCD_SCREEN_HEIGHT; i++) {
			// Instead of using lcd.clear(), clear one line at a time so there
			// is less screen flickr.

			if (i+(itemIndex/LCD_SCREEN_HEIGHT)*LCD_SCREEN_HEIGHT +1 > itemCount) {
				break;
			}

			lcd.setCursor(1,i);
			// Draw one page of items at a time
			drawItem(i+(itemIndex/LCD_SCREEN_HEIGHT)*LCD_SCREEN_HEIGHT, lcd);
		}
	}
	else {
		// Only need to clear the previous cursor
		lcd.setCursor(0,(lastDrawIndex%LCD_SCREEN_HEIGHT));
		lcd.write(' ');
	}

	lcd.setCursor(0,(itemIndex%LCD_SCREEN_HEIGHT));
	lcd.write('>');
	lastDrawIndex = itemIndex;
}

void Menu::reset() {
	firstItemIndex = 0;
	itemIndex = 0;
	lastDrawIndex = 255;

	resetState();
}

void Menu::resetState() {
}

void Menu::handleSelect(uint8_t index) {
}

void Menu::handleCancel() {
	// Remove ourselves from the menu list
        interface::popScreen();
}

void Menu::notifyButtonPressed(ButtonArray::ButtonName button) {
	switch (button) {
        case ButtonArray::ZERO:
        case ButtonArray::OK:
		handleSelect(itemIndex);
		break;
        case ButtonArray::CANCEL:
		handleCancel();
		break;
        case ButtonArray::YMINUS:
        case ButtonArray::ZMINUS:
		// increment index
		if (itemIndex < itemCount - 1) {
			itemIndex++;
		}
		break;
        case ButtonArray::YPLUS:
        case ButtonArray::ZPLUS:
		// decrement index
		if (itemIndex > firstItemIndex) {
			itemIndex--;
		}
		break;

        case ButtonArray::XMINUS:
        case ButtonArray::XPLUS:
		break;
	}
}


CancelBuildMenu::CancelBuildMenu() {
	itemCount = 4;
	reset();
}

void CancelBuildMenu::resetState() {
	itemIndex = 2;
	firstItemIndex = 2;
}

void CancelBuildMenu::drawItem(uint8_t index, LiquidCrystal& lcd) {
	static PROGMEM prog_uchar cancel[] = "Cancel Build?";
	static PROGMEM prog_uchar yes[] =   "Yes";
	static PROGMEM prog_uchar no[] =   "No";

	switch (index) {
	case 0:
		lcd.writeFromPgmspace(cancel);
		break;
	case 1:
		break;
	case 2:
		lcd.writeFromPgmspace(yes);
		break;
	case 3:
		lcd.writeFromPgmspace(no);
		break;
	}
}

void CancelBuildMenu::handleSelect(uint8_t index) {
	switch (index) {
	case 2:
		// Cancel build, returning to whatever menu came before monitor mode.
		// TODO: Cancel build.
		interface::popScreen();
		host::stopBuild();
		break;
	case 3:
		// Don't cancel, just close dialog.
                interface::popScreen();
		break;
	}
}


MainMenu::MainMenu() {
	itemCount = 7;
	reset();
}

void MainMenu::drawItem(uint8_t index, LiquidCrystal& lcd) {
	static PROGMEM prog_uchar monitor[] = "Monitor Mode";
	static PROGMEM prog_uchar build[] =   "Build from SD";
	static PROGMEM prog_uchar jog[] =     "Jog Mode";
	static PROGMEM prog_uchar preheat[] = "Preheat";
	static PROGMEM prog_uchar versions[] =   "Version";
	static PROGMEM prog_uchar snake[] =   "Snake Game";

	switch (index) {
	case 0:
		lcd.writeFromPgmspace(monitor);
		break;
	case 1:
		lcd.writeFromPgmspace(build);
		break;
	case 2:
		lcd.writeFromPgmspace(jog);
		break;
	case 3:
		lcd.writeFromPgmspace(preheat);
		break;
	case 4:
		// blank
		break;
	case 5:
		lcd.writeFromPgmspace(versions);
		break;
	case 6:
		lcd.writeFromPgmspace(snake);
		break;
	}
}

void MainMenu::handleSelect(uint8_t index) {
	switch (index) {
		case 0:
			// Show monitor build screen
                        interface::pushScreen(&monitorMode);
			break;
		case 1:
			// Show build from SD screen
                        interface::pushScreen(&sdMenu);
			break;
		case 2:
			// Show build from SD screen
                        interface::pushScreen(&jogger);
			break;
		case 3:
			// Show preheat menu
			interface::pushScreen(&preheatMenu);
			preheatMenu.fetchTargetTemps();
			break;
		case 4:
			// blank
			break;
		case 5:
			// Show build from SD screen
                        interface::pushScreen(&versionMode);
			break;
		case 6:
			// Show build from SD screen
                        interface::pushScreen(&snake);
			break;
		}
}

SDMenu::SDMenu() {
	reset();
}

void SDMenu::resetState() {
	itemCount = countFiles();
}

// Count the number of files on the SD card
uint8_t SDMenu::countFiles() {
	uint8_t count = 0;

	sdcard::SdErrorCode e;

	// First, reset the directory index
	e = sdcard::directoryReset();
	if (e != sdcard::SD_SUCCESS) {
		// TODO: Report error
		return 6;
	}

	const int MAX_FILE_LEN = 2;
	char fnbuf[MAX_FILE_LEN];

	// Count the files
	do {
		e = sdcard::directoryNextEntry(fnbuf,MAX_FILE_LEN);
		if (fnbuf[0] == '\0') {
			break;
		}

		// If it's a dot file, don't count it.
		if (fnbuf[0] == '.') {
		}
		else {
			count++;
		}
	} while (e == sdcard::SD_SUCCESS);

	// TODO: Check for error again?

	return count;
}

bool SDMenu::getFilename(uint8_t index, char buffer[], uint8_t buffer_size) {
	sdcard::SdErrorCode e;

	// First, reset the directory list
	e = sdcard::directoryReset();
	if (e != sdcard::SD_SUCCESS) {
                return false;
	}


	for(uint8_t i = 0; i < index+1; i++) {
		// Ignore dot-files
		do {
			e = sdcard::directoryNextEntry(buffer,buffer_size);
			if (buffer[0] == '\0') {
                                return false;
			}
		} while (e == sdcard::SD_SUCCESS && buffer[0] == '.');

		if (e != sdcard::SD_SUCCESS) {
                        return false;
		}
	}

        return true;
}

void SDMenu::drawItem(uint8_t index, LiquidCrystal& lcd) {
	if (index > itemCount - 1) {
		// TODO: report error
		return;
	}

	const uint8_t MAX_FILE_LEN = LCD_SCREEN_WIDTH;
	char fnbuf[MAX_FILE_LEN];

        if ( !getFilename(index, fnbuf, MAX_FILE_LEN) ) {
                // TODO: report error
		return;
	}

	uint8_t idx;
	for (idx = 0; (idx < MAX_FILE_LEN) && (fnbuf[idx] != 0); idx++) {
		lcd.write(fnbuf[idx]);
	}
}

void SDMenu::handleSelect(uint8_t index) {
	if (host::getHostState() != host::HOST_STATE_READY) {
		// TODO: report error
		return;
	}

	char* buildName = host::getBuildName();

        if ( !getFilename(index, buildName, host::MAX_FILE_LEN) ) {
		// TODO: report error
		return;
	}

        sdcard::SdErrorCode e;
	e = host::startBuildFromSD();
	if (e != sdcard::SD_SUCCESS) {
		// TODO: report error
		return;
	}
}


void Tool0TempSetScreen::reset() {
	value = eeprom::getEeprom8(eeprom::TOOL0_TEMP, 220);;
}

void Tool0TempSetScreen::update(LiquidCrystal& lcd, bool forceRedraw) {
	static PROGMEM prog_uchar message1[] = "Tool0 Targ Temp:";
	static PROGMEM prog_uchar message4[] = "Up/Dn/Ent to Set";

	if (forceRedraw) {
		lcd.clear();

		lcd.setCursor(0,0);
		lcd.writeFromPgmspace(message1);

		lcd.setCursor(0,3);
		lcd.writeFromPgmspace(message4);
	}


	// Redraw tool info
	lcd.setCursor(0,1);
	lcd.writeInt(value,3);
}

void Tool0TempSetScreen::notifyButtonPressed(ButtonArray::ButtonName button) {
	switch (button) {
        case ButtonArray::CANCEL:
		interface::popScreen();
		break;
        case ButtonArray::ZERO:
        case ButtonArray::OK:
		eeprom_write_byte((uint8_t*)eeprom::TOOL0_TEMP,value);
		interface::popScreen();
		break;
        case ButtonArray::ZPLUS:
		// increment more
		if (value <= 250) {
			value += 5;
		}
		break;
        case ButtonArray::ZMINUS:
		// decrement more
		if (value >= 5) {
			value -= 5;
		}
		break;
        case ButtonArray::YPLUS:
		// increment less
		if (value <= 254) {
			value += 1;
		}
		break;
        case ButtonArray::YMINUS:
		// decrement less
		if (value >= 1) {
			value -= 1;
		}
		break;

        case ButtonArray::XMINUS:
        case ButtonArray::XPLUS:
		break;
	}
}


void PlatformTempSetScreen::reset() {
	value = eeprom::getEeprom8(eeprom::PLATFORM_TEMP, 110);;
}

void PlatformTempSetScreen::update(LiquidCrystal& lcd, bool forceRedraw) {
	static PROGMEM prog_uchar message1[] = "Bed Target Temp:";
	static PROGMEM prog_uchar message4[] = "Up/Dn/Ent to Set";

	if (forceRedraw) {
		lcd.clear();

		lcd.setCursor(0,0);
		lcd.writeFromPgmspace(message1);

		lcd.setCursor(0,3);
		lcd.writeFromPgmspace(message4);
	}


	// Redraw tool info
	lcd.setCursor(0,1);
	lcd.writeInt(value,3);
}

void PlatformTempSetScreen::notifyButtonPressed(ButtonArray::ButtonName button) {
	switch (button) {
        case ButtonArray::CANCEL:
		interface::popScreen();
		break;
        case ButtonArray::ZERO:
        case ButtonArray::OK:
		eeprom_write_byte((uint8_t*)eeprom::PLATFORM_TEMP,value);
		interface::popScreen();
		break;
        case ButtonArray::ZPLUS:
		// increment more
		if (value <= 250) {
			value += 5;
		}
		break;
        case ButtonArray::ZMINUS:
		// decrement more
		if (value >= 5) {
			value -= 5;
		}
		break;
        case ButtonArray::YPLUS:
		// increment less
		if (value <= 254) {
			value += 1;
		}
		break;
        case ButtonArray::YMINUS:
		// decrement less
		if (value >= 1) {
			value -= 1;
		}
		break;

        case ButtonArray::XMINUS:
        case ButtonArray::XPLUS:
		break;
	}
}


PreheatMenu::PreheatMenu() {
	itemCount = 4;
	reset();
}

void PreheatMenu::fetchTargetTemps() {
	OutPacket responsePacket;
	if (extruderControl(SLAVE_CMD_GET_SP, EXTDR_CMD_GET, responsePacket, 0)) {
		tool0Temp = responsePacket.read16(1);
	}
	if (extruderControl(SLAVE_CMD_GET_PLATFORM_SP, EXTDR_CMD_GET, responsePacket, 0)) {
		platformTemp = responsePacket.read16(1);
	}
}

void PreheatMenu::drawItem(uint8_t index, LiquidCrystal& lcd) {
	static PROGMEM prog_uchar heat[]     = "Heat ";
	static PROGMEM prog_uchar cool[]     = "Cool ";
	static PROGMEM prog_uchar tool0[]    = "Tool0";
	static PROGMEM prog_uchar platform[] = "Bed";
	static PROGMEM prog_uchar tool0set[] = "Set Tool0 Temp";
	static PROGMEM prog_uchar platset[]  = "Set Bed Temp";

	switch (index) {
	case 0:
		fetchTargetTemps();
		if (tool0Temp > 0) {
			lcd.writeFromPgmspace(cool);
		} else {
			lcd.writeFromPgmspace(heat);
		}
		lcd.writeFromPgmspace(tool0);
		break;
	case 1:
		if (platformTemp > 0) {
			lcd.writeFromPgmspace(cool);
		} else {
			lcd.writeFromPgmspace(heat);
		}
		lcd.writeFromPgmspace(platform);
		break;
	case 2:
		lcd.writeFromPgmspace(tool0set);
		break;
	case 3:
		lcd.writeFromPgmspace(platset);
		break;
	}
}

void PreheatMenu::handleSelect(uint8_t index) {
	OutPacket responsePacket;
	switch (index) {
		case 0:
			// Toggle Extruder heater on/off
			if (tool0Temp > 0) {
				extruderControl(SLAVE_CMD_SET_TEMP, EXTDR_CMD_SET, responsePacket, 0);
			} else {
				uint8_t value = eeprom::getEeprom8(eeprom::TOOL0_TEMP, 220);
				extruderControl(SLAVE_CMD_SET_TEMP, EXTDR_CMD_SET, responsePacket, (uint16_t)value);
			}
			fetchTargetTemps();
			lastDrawIndex = 255; // forces redraw.
			break;
		case 1:
			// Toggle Platform heater on/off
			if (platformTemp > 0) {
				extruderControl(SLAVE_CMD_SET_PLATFORM_TEMP, EXTDR_CMD_SET, responsePacket, 0);
			} else {
				uint8_t value = eeprom::getEeprom8(eeprom::PLATFORM_TEMP, 110);
				extruderControl(SLAVE_CMD_SET_PLATFORM_TEMP, EXTDR_CMD_SET, responsePacket, value);
			}
			fetchTargetTemps();
			lastDrawIndex = 255; // forces redraw.
			break;
		case 2:
			// Show Extruder Temperature Setting Screen
                        interface::pushScreen(&tool0TempSetScreen);
			break;
		case 3:
			// Show Platform Temperature Setting Screen
                        interface::pushScreen(&platTempSetScreen);
			break;
		}
}

#endif
