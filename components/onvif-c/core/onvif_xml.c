/*
 * onvif-c core — canonical SOAP response builders.
 *
 * Extracted verbatim from the MiBee Cam firmware (field-proven against
 * the MiBee NVR and Hikvision-class clients); see onvif_xml.h for the
 * byte-stability contract.
 */

#include "onvif_xml.h"

#include <stdio.h>

#define NS_EV  "http://www.onvif.org/ver10/events/wsdl"
#define NS_WSN "http://docs.oasis-open.org/wsn/b-2"
#define NS_WSA "http://www.w3.org/2005/08/addressing"
#define NS_TT  "http://www.onvif.org/ver10/schema"

/* ------------------------------------------------------------------ */
/*  Device service                                                     */
/* ------------------------------------------------------------------ */

int onvif_xml_system_date_and_time(char *buf, size_t n, const struct tm *utc)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<soap:Body>"
        "<tds:GetSystemDateAndTimeResponse>"
        "<tds:SystemDateAndTime>"
        "<tt:DateTimeType>NTP</tt:DateTimeType>"
        "<tt:DaylightSavings>false</tt:DaylightSavings>"
        "<tt:TimeZone>"
        "<tt:TZ>UTC</tt:TZ>"
        "</tt:TimeZone>"
        "<tt:UTCDateTime>"
        "<tt:Time>"
        "<tt:Hour>%d</tt:Hour>"
        "<tt:Minute>%d</tt:Minute>"
        "<tt:Second>%d</tt:Second>"
        "</tt:Time>"
        "<tt:Date>"
        "<tt:Year>%d</tt:Year>"
        "<tt:Month>%d</tt:Month>"
        "<tt:Day>%d</tt:Day>"
        "</tt:Date>"
        "</tt:UTCDateTime>"
        "</tds:SystemDateAndTime>"
        "</tds:GetSystemDateAndTimeResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        utc->tm_hour, utc->tm_min, utc->tm_sec,
        utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday);
}

int onvif_xml_device_information(char *buf, size_t n, const char *manufacturer,
                                 const char *model, const char *firmware,
                                 const char *serial, const char *hardware_id)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
        "<soap:Body>"
        "<tds:GetDeviceInformationResponse>"
        "<tds:Manufacturer>%s</tds:Manufacturer>"
        "<tds:Model>%s</tds:Model>"
        "<tds:FirmwareVersion>%s</tds:FirmwareVersion>"
        "<tds:SerialNumber>%s</tds:SerialNumber>"
        "<tds:HardwareId>%s</tds:HardwareId>"
        "</tds:GetDeviceInformationResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        manufacturer, model, firmware, serial, hardware_id);
}

int onvif_xml_capabilities(char *buf, size_t n, const char *ip, bool events)
{
    int off = snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<soap:Body>"
        "<tds:GetCapabilitiesResponse>"
        "<tds:Capabilities>"
        "<tt:Device>"
        "<tt:XAddr>http://%s:80/onvif/device_service</tt:XAddr>"
        "</tt:Device>"
        "<tt:Media>"
        "<tt:XAddr>http://%s:80/onvif/media_service</tt:XAddr>"
        "</tt:Media>",
        ip, ip);
    if (off < 0 || (size_t)off >= n) {
        return off;
    }
    if (events) {
        off += snprintf(buf + off, n - off,
            "<tt:Events>"
            "<tt:XAddr>http://%s:80/onvif/events_service</tt:XAddr>"
            "</tt:Events>", ip);
        if (off < 0 || (size_t)off >= n) {
            return off;
        }
    }
    off += snprintf(buf + off, n - off,
        "<tt:Analytics>"
        "<tt:XAddr>http://%s:80/onvif/analytics_service</tt:XAddr>"
        "</tt:Analytics>"
        "</tds:Capabilities>"
        "</tds:GetCapabilitiesResponse>"
        "</soap:Body>"
        "</soap:Envelope>", ip);
    return off;
}

/* ------------------------------------------------------------------ */
/*  Media service                                                      */
/* ------------------------------------------------------------------ */

int onvif_xml_profiles(char *buf, size_t n, int frame_rate)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<soap:Body>"
        "<trt:GetProfilesResponse>"
        "<trt:Profiles token=\"MainStream\" fixed=\"true\">"
        "<tt:Name>MainStream</tt:Name>"
        "<tt:VideoSourceConfiguration>"
        "<tt:Name>VideoSource_1</tt:Name>"
        "<tt:UseCount>1</tt:UseCount>"
        "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
        "</tt:VideoSourceConfiguration>"
        "<tt:VideoEncoderConfiguration>"
        "<tt:Name>VideoEncoder_1</tt:Name>"
        "<tt:Encoding>JPEG</tt:Encoding>"
        "<tt:Resolution>"
        "<tt:Width>640</tt:Width>"
        "<tt:Height>480</tt:Height>"
        "</tt:Resolution>"
        "<tt:Quality>5</tt:Quality>"
        "<tt:RateControl>"
        "<tt:FrameRateLimit>%d</tt:FrameRateLimit>"
        "<tt:EncodingInterval>1</tt:EncodingInterval>"
        "<tt:BitrateLimit>4096</tt:BitrateLimit>"
        "</tt:RateControl>"
        "</tt:VideoEncoderConfiguration>"
        "</trt:Profiles>"
        "</trt:GetProfilesResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        frame_rate);
}

int onvif_xml_stream_uri(char *buf, size_t n, const char *uri)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<soap:Body>"
        "<trt:GetStreamUriResponse>"
        "<trt:MediaUri>"
        "<tt:Uri>%s</tt:Uri>"
        "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
        "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
        "<tt:Timeout>PT10S</tt:Timeout>"
        "</trt:MediaUri>"
        "</trt:GetStreamUriResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        uri);
}

int onvif_xml_snapshot_uri(char *buf, size_t n, const char *uri)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<soap:Body>"
        "<trt:GetSnapshotUriResponse>"
        "<trt:MediaUri>"
        "<tt:Uri>%s</tt:Uri>"
        "</trt:MediaUri>"
        "</trt:GetSnapshotUriResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        uri);
}

/* ------------------------------------------------------------------ */
/*  Faults                                                             */
/* ------------------------------------------------------------------ */

int onvif_xml_fault_action_not_supported(char *buf, size_t n)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:ter=\"http://www.onvif.org/ver10/error\">"
        "<soap:Body>"
        "<soap:Fault>"
        "<soap:Code>"
        "<soap:Value>soap:Sender</soap:Value>"
        "<soap:Subcode>"
        "<soap:Value>ter:ActionNotSupported</soap:Value>"
        "</soap:Subcode>"
        "</soap:Code>"
        "<soap:Reason>"
        "<soap:Text xml:lang=\"en\">Action not supported</soap:Text>"
        "</soap:Reason>"
        "</soap:Fault>"
        "</soap:Body>"
        "</soap:Envelope>");
}

/* ------------------------------------------------------------------ */
/*  Events service (Pull-Point)                                        */
/* ------------------------------------------------------------------ */

int onvif_xml_create_pull_point_response(char *buf, size_t n, const char *ip,
                                         const char *now, const char *termination)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<tev:CreatePullPointSubscriptionResponse xmlns:tev=\"" NS_EV "\" "
        "xmlns:wsa=\"" NS_WSA "\" xmlns:wsnt=\"" NS_WSN "\">"
        "<tev:SubscriptionReference>"
        "<wsa:Address>http://%s:80/onvif/events_service</wsa:Address>"
        "</tev:SubscriptionReference>"
        "<wsnt:CurrentTime>%s</wsnt:CurrentTime>"
        "<wsnt:TerminationTime>%s</wsnt:TerminationTime>"
        "</tev:CreatePullPointSubscriptionResponse>"
        "</s:Body></s:Envelope>",
        ip, now, termination);
}

int onvif_xml_renew_response(char *buf, size_t n, const char *termination)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<wsnt:RenewResponse xmlns:wsnt=\"" NS_WSN "\">"
        "<wsnt:TerminationTime>%s</wsnt:TerminationTime>"
        "</wsnt:RenewResponse>"
        "</s:Body></s:Envelope>",
        termination);
}

const char *onvif_xml_unsubscribe_response(void)
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<wsnt:UnsubscribeResponse xmlns:wsnt=\"" NS_WSN "\"/>"
        "</s:Body></s:Envelope>";
}

int onvif_xml_events_fault(char *buf, size_t n, const char *subcode,
                           const char *text)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body><s:Fault>"
        "<s:Code><s:Value>s:Sender</s:Value>"
        "<s:Subcode><s:Value>%s</s:Value></s:Subcode></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason>"
        "</s:Fault></s:Body></s:Envelope>",
        subcode, text);
}

int onvif_xml_pull_open(char *buf, size_t n)
{
    return snprintf(buf, n,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<tev:PullMessagesResponse xmlns:tev=\"" NS_EV "\" "
        "xmlns:wsnt=\"" NS_WSN "\" xmlns:tt=\"" NS_TT "\">");
}

int onvif_xml_pull_event(char *buf, size_t n, const char *utc, bool active,
                         unsigned score)
{
    return snprintf(buf, n,
        "<wsnt:NotificationMessage>"
        "<wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">"
        "tns1:VideoSource/MotionAlarm</wsnt:Topic>"
        "<wsnt:Message><tt:Message UtcTime=\"%s\">"
        "<tt:Source><tt:SimpleItem Name=\"Source\" Value=\"CSI\"/></tt:Source>"
        "<tt:Data>"
        "<tt:SimpleItem Name=\"State\" Value=\"%s\"/>"
        "<tt:SimpleItem Name=\"Score\" Value=\"%u\"/>"
        "</tt:Data></tt:Message></wsnt:Message>"
        "</wsnt:NotificationMessage>",
        utc, active ? "true" : "false", score);
}

int onvif_xml_pull_close(char *buf, size_t n, const char *now,
                         const char *termination)
{
    return snprintf(buf, n,
        "<tev:CurrentTime>%s</tev:CurrentTime>"
        "<tev:TerminationTime>%s</tev:TerminationTime>"
        "</tev:PullMessagesResponse></s:Body></s:Envelope>",
        now, termination);
}
