#!/bin/sh

# Copyright 2011 Dvir Volk <dvirsk at gmail dot com>. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   1. Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
#
#   2. Redistributions in binary form must reproduce the above copyright
#   notice, this list of conditions and the following disclaimer in the
#   documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
# WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
# EVENT SHALL Dvir Volk OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
# OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
# NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
# EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
################################################################################
#
# Service installer for the server, runs interactively by default.
#
# To run this script non-interactively (for automation/provisioning purposes),
# feed the variables into the script. Any missing variables will be prompted!
# Tip: Environment variables also support command substitution (see SERVER_EXECUTABLE)
#
# Example:
#
# sudo SERVER_PORT=1234 \
# 		 SERVER_CONFIG_FILE=/etc/valkey/1234.conf \
# 		 SERVER_LOG_FILE=/var/log/valkey_1234.log \
# 		 SERVER_DATA_DIR=/var/lib/valkey/1234 \
# 		 SERVER_EXECUTABLE=`command -v valkey-server` ./utils/install_server.sh
#
# This generates a server config file and an /etc/init.d script, and installs them.
#
# /!\ This script should be run as root
#
# NOTE: This script will not work on Mac OSX.
#       It supports Debian and Ubuntu Linux.
#
################################################################################

die () {
	echo "ERROR: $1. Aborting!"
	exit 1
}


#Absolute path to this script
SCRIPT=$(readlink -f $0)
#Absolute path this script is in
SCRIPTPATH=$(dirname $SCRIPT)

#Initial defaults
_SERVER_PORT=6379
_MANUAL_EXECUTION=false

echo "Welcome to the valkey service installer"
echo "This script will help you easily set up a running valkey server"
echo

#check for root user
if [ "$(id -u)" -ne 0 ] ; then
	echo "You must run this script as root. Sorry!"
	exit 1
fi

#bail if this system is managed by systemd
_pid_1_exe="$(readlink -f /proc/1/exe)"
if [ "${_pid_1_exe##*/}" = systemd ]
then
	echo "This systems seems to use systemd."
	echo "Please take a look at the provided example service unit files in this directory, and adapt and install them. Sorry!"
	exit 1
fi
unset _pid_1_exe

if ! echo $SERVER_PORT | egrep -q '^[0-9]+$' ; then
	_MANUAL_EXECUTION=true
	#Read the server port
	read  -p "Please select the server port for this instance: [$_SERVER_PORT] " SERVER_PORT
	if ! echo $SERVER_PORT | egrep -q '^[0-9]+$' ; then
		echo "Selecting default: $_SERVER_PORT"
		SERVER_PORT=$_SERVER_PORT
	fi
fi

if [ -z "$SERVER_CONFIG_FILE" ] ; then
	_MANUAL_EXECUTION=true
	#read the server config file
	_SERVER_CONFIG_FILE="/etc/valkey/$SERVER_PORT.conf"
	read -p "Please select the valkey config file name [$_SERVER_CONFIG_FILE] " SERVER_CONFIG_FILE
	if [ -z "$SERVER_CONFIG_FILE" ] ; then
		SERVER_CONFIG_FILE=$_SERVER_CONFIG_FILE
		echo "Selected default - $SERVER_CONFIG_FILE"
	fi
fi

if [ -z "$SERVER_LOG_FILE" ] ; then
	_MANUAL_EXECUTION=true
	#read the server log file path
	_SERVER_LOG_FILE="/var/log/valkey_$SERVER_PORT.log"
	read -p "Please select the server log file name [$_SERVER_LOG_FILE] " SERVER_LOG_FILE
	if [ -z "$SERVER_LOG_FILE" ] ; then
		SERVER_LOG_FILE=$_SERVER_LOG_FILE
		echo "Selected default - $SERVER_LOG_FILE"
	fi
fi

if [ -z "$SERVER_DATA_DIR" ] ; then
	_MANUAL_EXECUTION=true
	#get the server data directory
	_SERVER_DATA_DIR="/var/lib/valkey/$SERVER_PORT"
	read -p "Please select the data directory for this instance [$_SERVER_DATA_DIR] " SERVER_DATA_DIR
	if [ -z "$SERVER_DATA_DIR" ] ; then
		SERVER_DATA_DIR=$_SERVER_DATA_DIR
		echo "Selected default - $SERVER_DATA_DIR"
	fi
fi

if [ ! -x "$SERVER_EXECUTABLE" ] ; then
	_MANUAL_EXECUTION=true
	#get the server executable path
	_SERVER_EXECUTABLE=`command -v valkey-server`
	read -p "Please select the valkey executable path [$_SERVER_EXECUTABLE] " SERVER_EXECUTABLE
	if [ ! -x "$SERVER_EXECUTABLE" ] ; then
		SERVER_EXECUTABLE=$_SERVER_EXECUTABLE

		if [ ! -x "$SERVER_EXECUTABLE" ] ; then
			echo "Mmmmm...  it seems like you don't have a valkey executable. Did you run make install yet?"
			exit 1
		fi
	fi
fi

#check the default for valkey cli
CLI_EXEC=`command -v valkey-cli`
if [ -z "$CLI_EXEC" ] ; then
	CLI_EXEC=`dirname $SERVER_EXECUTABLE`"/valkey-cli"
fi

echo "Selected config:"

echo "Port           : $SERVER_PORT"
echo "Config file    : $SERVER_CONFIG_FILE"
echo "Log file       : $SERVER_LOG_FILE"
echo "Data dir       : $SERVER_DATA_DIR"
echo "Executable     : $SERVER_EXECUTABLE"
echo "Cli Executable : $CLI_EXEC"

if $_MANUAL_EXECUTION == true ; then
	read -p "Is this ok? Then press ENTER to go on or Ctrl-C to abort." _UNUSED_
fi

mkdir -p `dirname "$SERVER_CONFIG_FILE"` || die "Could not create valkey config directory"
mkdir -p `dirname "$SERVER_LOG_FILE"` || die "Could not create valkey log dir"
mkdir -p "$SERVER_DATA_DIR" || die "Could not create valkey data directory"

#render the templates
TMP_FILE="/tmp/${SERVER_PORT}.conf"
DEFAULT_CONFIG="${SCRIPTPATH}/../valkey.conf"
INIT_TPL_FILE="${SCRIPTPATH}/valkey_init_script.tpl"
INIT_SCRIPT_DEST="/etc/init.d/valkey_${SERVER_PORT}"
PIDFILE="/var/run/valkey_${SERVER_PORT}.pid"

if [ ! -f "$DEFAULT_CONFIG" ]; then
	echo "Mmmmm... the default config is missing. Did you switch to the utils directory?"
	exit 1
fi

#Generate config file from the default config file as template
#changing only the stuff we're controlling from this script
echo "## Generated by install_server.sh ##" > $TMP_FILE

read -r SED_EXPR <<-EOF
s#^port .\+#port ${SERVER_PORT}#; \
s#^logfile .\+#logfile ${SERVER_LOG_FILE}#; \
s#^dir .\+#dir ${SERVER_DATA_DIR}#; \
s#^pidfile .\+#pidfile ${PIDFILE}#; \
s#^daemonize no#daemonize yes#;
EOF
sed "$SED_EXPR" $DEFAULT_CONFIG >> $TMP_FILE

#cat $TPL_FILE | while read line; do eval "echo \"$line\"" >> $TMP_FILE; done
cp $TMP_FILE $SERVER_CONFIG_FILE || die "Could not write valkey config file $SERVER_CONFIG_FILE"

#Generate sample script from template file
rm -f $TMP_FILE

#we hard code the configs here to avoid issues with templates containing env vars
#kinda lame but works!
VALKEY_INIT_HEADER=\
"#!/bin/sh\n
#Configurations injected by install_server below....\n\n
EXEC=$SERVER_EXECUTABLE\n
CLIEXEC=$CLI_EXEC\n
PIDFILE=\"$PIDFILE\"\n
CONF=\"$SERVER_CONFIG_FILE\"\n\n
VALKEYPORT=\"$SERVER_PORT\"\n\n
###############\n\n"

VALKEY_CHKCONFIG_INFO=\
"# REDHAT chkconfig header\n\n
# chkconfig: - 58 74\n
# description: valkey_${SERVER_PORT} is the valkey daemon.\n
### BEGIN INIT INFO\n
# Provides: valkey_6379\n
# Required-Start: \$network \$local_fs \$remote_fs\n
# Required-Stop: \$network \$local_fs \$remote_fs\n
# Default-Start: 2 3 4 5\n
# Default-Stop: 0 1 6\n
# Should-Start: \$syslog \$named\n
# Should-Stop: \$syslog \$named\n
# Short-Description: start and stop valkey_${SERVER_PORT}\n
# Description: Valkey daemon\n
### END INIT INFO\n\n"

if command -v chkconfig >/dev/null; then
	#if we're a box with chkconfig on it we want to include info for chkconfig
	echo "$VALKEY_INIT_HEADER" "$VALKEY_CHKCONFIG_INFO" > $TMP_FILE && cat $INIT_TPL_FILE >> $TMP_FILE || die "Could not write init script to $TMP_FILE"
else
	#combine the header and the template (which is actually a static footer)
	echo "$VALKEY_INIT_HEADER" > $TMP_FILE && cat $INIT_TPL_FILE >> $TMP_FILE || die "Could not write init script to $TMP_FILE"
fi

###
# Generate sample script from template file
# - No need to check which system we are on. The init info are comments and
#   do not interfere with update_rc.d systems. Additionally:
#     Ubuntu/debian by default does not come with chkconfig, but does issue a
#     warning if init info is not available.

cat > ${TMP_FILE} <<EOT
#!/bin/sh
#Configurations injected by install_server below....

EXEC=$SERVER_EXECUTABLE
CLIEXEC=$CLI_EXEC
PIDFILE=$PIDFILE
CONF="$SERVER_CONFIG_FILE"
VALKEYPORT="$SERVER_PORT"
###############
# SysV Init Information
# chkconfig: - 58 74
# description: valkey_${SERVER_PORT} is the valkey daemon.
### BEGIN INIT INFO
# Provides: valkey_${SERVER_PORT}
# Required-Start: \$network \$local_fs \$remote_fs
# Required-Stop: \$network \$local_fs \$remote_fs
# Default-Start: 2 3 4 5
# Default-Stop: 0 1 6
# Should-Start: \$syslog \$named
# Should-Stop: \$syslog \$named
# Short-Description: start and stop valkey_${SERVER_PORT}
# Description: Valkey daemon
### END INIT INFO

EOT
cat ${INIT_TPL_FILE} >> ${TMP_FILE}

#copy to /etc/init.d
cp $TMP_FILE $INIT_SCRIPT_DEST && \
	chmod +x $INIT_SCRIPT_DEST || die "Could not copy valkey init script to  $INIT_SCRIPT_DEST"
echo "Copied $TMP_FILE => $INIT_SCRIPT_DEST"

#Install the service
echo "Installing service..."
if command -v chkconfig >/dev/null 2>&1; then
	# we're chkconfig, so lets add to chkconfig and put in runlevel 345
	chkconfig --add valkey_${SERVER_PORT} && echo "Successfully added to chkconfig!"
	chkconfig --level 345 valkey_${SERVER_PORT} on && echo "Successfully added to runlevels 345!"
elif command -v update-rc.d >/dev/null 2>&1; then
	#if we're not a chkconfig box assume we're able to use update-rc.d
	update-rc.d valkey_${SERVER_PORT} defaults && echo "Success!"
else
	echo "No supported init tool found."
fi

/etc/init.d/valkey_$SERVER_PORT start || die "Failed starting service..."

#tada
echo "Installation successful!"
exit 0
