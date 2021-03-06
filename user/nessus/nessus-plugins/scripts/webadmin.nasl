#
# (C) Michel Arboi <arboi@alussinan.org>
#
#
# References:
# http://www.kamborio.com/?Section=Articles&Mode=select&ID=55
#
# From: "Mark Litchfield" <mark@ngssoftware.com>
# To: bugtraq@securityfocus.com, vulnwatch@vulnwatch.org, 
#   vulndb@securityfocus.com
# Date: Tue, 24 Jun 2003 15:22:21 -0700
# Subject: Remote Buffer Overrun WebAdmin.exe
#


if(description)
{
 script_id(11771);
 script_version ("$Revision: 1.2 $");

 name["english"] = "WebAdmin detection";

 script_name(english:name["english"]);
 
 desc["english"] = "
webadmin.dll was found on your web server. 
Old versions of this CGI suffered from numerous problems:
 - installation path disclosure
 - directory traversal, allowing anybody with 
   administrative permission on WebAdmin to read any file
 - buffer overflow, allowing anybody to run arbitrary code on
   your server with SYSTEM privileges.

*** Note that no attack was performed, and the version number was
*** not checked, so this might be a false alert

Solution : Upgrade to the latest version if necessary
Risk factor : Serious";

 script_description(english:desc["english"]);
 
 summary["english"] = "Checks for the presence of webadmin.dll";
 
 script_summary(english:summary["english"]);
 
 script_category(ACT_GATHER_INFO);
 
 
 script_copyright(english:"This script is Copyright (C) 2003 Michel Arboi");
 family["english"] = "CGI abuses";
 family["francais"] = "Abus de CGI";
 script_family(english:family["english"], francais:family["francais"]);
 script_dependencie("find_service.nes", "no404.nasl");
 script_require_ports("Services/www", 80);
 exit(0);
}

#

port = is_cgi_installed("webadmin.dll");
if (port) security_warning(port);
