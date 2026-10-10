/*
 * onvif-c core — canonical SOAP response builders.
 *
 * Extracted verbatim from the MiBee Cam firmware (field-proven against
 * the MiBee NVR and Hikvision-class clients); see onvif_xml.h for the
 * byte-stability contract.
 */

#include "onvif_xml.h"

#include <stdio.h>
#include <string.h>

#define NS_EV  "http://www.onvif.org/ver10/events/wsdl"
#define NS_WSN "http://docs.oasis-open.org/wsn/b-2"
#define NS_WSA "http://www.w3.org/2005/08/addressing"
#define NS_TT  "http://www.onvif.org/ver10/schema"

/* ------------------------------------------------------------------ */
/*  Device service                                                     */
/* ------------------------------------------------------------------ */

int onvif_xml_system_date_and_time(char *buf, size_t n, const struct tm *utc, bool ntp_active,
                                   const char *tz)
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
                    "<tt:DateTimeType>%s</tt:DateTimeType>"
                    "<tt:DaylightSavings>false</tt:DaylightSavings>"
                    "<tt:TimeZone>"
                    "<tt:TZ>%s</tt:TZ>"
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
                    ntp_active ? "NTP" : "MANUAL", (tz && tz[0]) ? tz : "UTC", utc->tm_hour,
                    utc->tm_min, utc->tm_sec, utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday);
}

int onvif_xml_device_information(char *buf, size_t n, const char *manufacturer, const char *model,
                                 const char *firmware, const char *serial, const char *hardware_id)
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

int onvif_xml_capabilities(char *buf, size_t n, const char *ip, unsigned port, bool events)
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
                       "<tt:XAddr>http://%s:%u/onvif/device_service</tt:XAddr>"
                       "</tt:Device>"
                       "<tt:Media>"
                       "<tt:XAddr>http://%s:%u/onvif/media_service</tt:XAddr>"
                       "</tt:Media>",
                       ip, port, ip, port);
    if (off < 0 || (size_t)off >= n) {
        return off;
    }
    if (events) {
        off += snprintf(buf + off, n - off,
                        "<tt:Events>"
                        "<tt:XAddr>http://%s:%u/onvif/events_service</tt:XAddr>"
                        "</tt:Events>",
                        ip, port);
        if (off < 0 || (size_t)off >= n) {
            return off;
        }
    }
    off += snprintf(buf + off, n - off,
                    "<tt:Analytics>"
                    "<tt:XAddr>http://%s:%u/onvif/analytics_service</tt:XAddr>"
                    "</tt:Analytics>"
                    "</tds:Capabilities>"
                    "</tds:GetCapabilitiesResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    ip, port);
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

int onvif_xml_fault_invalid(char *buf, size_t n, const char *subcode, const char *reason)
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
                    "<soap:Value>%s</soap:Value>"
                    "</soap:Subcode>"
                    "</soap:Code>"
                    "<soap:Reason>"
                    "<soap:Text xml:lang=\"en\">%s</soap:Text>"
                    "</soap:Reason>"
                    "</soap:Fault>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    subcode ? subcode : "ter:InvalidArgVal", reason ? reason : "invalid request");
}

int onvif_xml_set_ntp_response(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
                    "<soap:Body>"
                    "<tds:SetNTPResponse/>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}

/* ------------------------------------------------------------------ */
/*  Events service (Pull-Point)                                        */
/* ------------------------------------------------------------------ */

int onvif_xml_create_pull_point_response(char *buf, size_t n, const char *ip, unsigned port,
                                         const char *now, const char *termination)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
                    "<s:Body>"
                    "<tev:CreatePullPointSubscriptionResponse xmlns:tev=\"" NS_EV "\" "
                    "xmlns:wsa=\"" NS_WSA "\" xmlns:wsnt=\"" NS_WSN "\">"
                    "<tev:SubscriptionReference>"
                    "<wsa:Address>http://%s:%u/onvif/events_service</wsa:Address>"
                    "</tev:SubscriptionReference>"
                    "<wsnt:CurrentTime>%s</wsnt:CurrentTime>"
                    "<wsnt:TerminationTime>%s</wsnt:TerminationTime>"
                    "</tev:CreatePullPointSubscriptionResponse>"
                    "</s:Body></s:Envelope>",
                    ip, port, now, termination);
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
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
           "<s:Body>"
           "<wsnt:UnsubscribeResponse xmlns:wsnt=\"" NS_WSN "\"/>"
           "</s:Body></s:Envelope>";
}

int onvif_xml_events_fault(char *buf, size_t n, const char *subcode, const char *text)
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

int onvif_xml_pull_event(char *buf, size_t n, const char *utc, bool active, unsigned score)
{
    return snprintf(
        buf, n,
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

int onvif_xml_pull_close(char *buf, size_t n, const char *now, const char *termination)
{
    return snprintf(buf, n,
                    "<tev:CurrentTime>%s</tev:CurrentTime>"
                    "<tev:TerminationTime>%s</tev:TerminationTime>"
                    "</tev:PullMessagesResponse></s:Body></s:Envelope>",
                    now, termination);
}

/* ------------------------------------------------------------------ */
/*  Device service completions (issue #13)                             */
/* ------------------------------------------------------------------ */

int onvif_xml_services(char *buf, size_t n, const char *ip, unsigned port, bool events)
{
    int off = snprintf(buf, n,
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<soap:Envelope"
                       " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                       " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
                       " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
                       "<soap:Body>"
                       "<tds:GetServicesResponse>"
                       "<tds:Service>"
                       "<tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
                       "<tds:XAddr>http://%s:%u/onvif/device_service</tds:XAddr>"
                       "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version>"
                       "</tds:Service>"
                       "<tds:Service>"
                       "<tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
                       "<tds:XAddr>http://%s:%u/onvif/media_service</tds:XAddr>"
                       "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version>"
                       "</tds:Service>"
                       "<tds:Service>"
                       "<tds:Namespace>http://www.onvif.org/ver20/analytics/wsdl</tds:Namespace>"
                       "<tds:XAddr>http://%s:%u/onvif/analytics_service</tds:XAddr>"
                       "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version>"
                       "</tds:Service>",
                       ip, port, ip, port, ip, port);
    if (off < 0 || (size_t)off >= n) {
        return off;
    }
    if (events) {
        off += snprintf(buf + off, n - off,
                        "<tds:Service>"
                        "<tds:Namespace>http://www.onvif.org/ver10/events/wsdl</tds:Namespace>"
                        "<tds:XAddr>http://%s:%u/onvif/events_service</tds:XAddr>"
                        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version>"
                        "</tds:Service>",
                        ip, port);
        if (off < 0 || (size_t)off >= n) {
            return off;
        }
    }
    off += snprintf(buf + off, n - off,
                    "<tds:Service>"
                    "<tds:Namespace>http://www.onvif.org/ver20/media/wsdl</tds:Namespace>"
                    "<tds:XAddr>http://%s:%u/onvif/media2_service</tds:XAddr>"
                    "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version>"
                    "</tds:Service>"
                    "</tds:GetServicesResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    ip, port);
    return off;
}

int onvif_xml_get_scopes(char *buf, size_t n, const char *scopes)
{
    /* Element form per the WSDL (ScopeDef + ScopeItem per entry) — the
     * attribute form some firmware emits is not schema-valid. */
    int off = snprintf(buf, n,
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<soap:Envelope"
                       " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                       " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
                       " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
                       "<soap:Body>"
                       "<tds:GetScopesResponse>");
    if (off < 0 || (size_t)off >= n) {
        return off;
    }
    const char *p = scopes ? scopes : "";
    while (*p) {
        while (*p == ' ')
            p++;
        const char *end = strchr(p, ' ');
        size_t      len = end ? (size_t)(end - p) : strlen(p);
        if (len == 0)
            break;
        off += snprintf(buf + off, n - off,
                        "<tt:Scopes>"
                        "<tt:ScopeDef>Fixed</tt:ScopeDef>"
                        "<tt:ScopeItem>%.*s</tt:ScopeItem>"
                        "</tt:Scopes>",
                        (int)len, p);
        if (off < 0 || (size_t)off >= n) {
            return off;
        }
        p += len;
    }
    off += snprintf(buf + off, n - off,
                    "</tds:GetScopesResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>");
    return off;
}

int onvif_xml_system_reboot(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
                    "<soap:Body>"
                    "<tds:SystemRebootResponse>"
                    "<tds:Message>Device rebooting</tds:Message>"
                    "</tds:SystemRebootResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}

int onvif_xml_set_system_date_and_time_ack(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
                    "<soap:Body>"
                    "<tds:SetSystemDateAndTimeResponse/>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}

int onvif_xml_device_service_capabilities(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
                    "<soap:Body>"
                    "<tds:GetServiceCapabilitiesResponse>"
                    "<tds:Capabilities Network=\"false\" System=\"false\"/>"
                    "</tds:GetServiceCapabilitiesResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}

/* ------------------------------------------------------------------ */
/*  Media service completions (issue #14)                              */
/* ------------------------------------------------------------------ */

int onvif_xml_video_sources(char *buf, size_t n, const char *token, int width, int height,
                            int frame_rate)
{
    return snprintf(
        buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<soap:Body>"
        "<trt:GetVideoSourcesResponse>"
        "<trt:VideoSources token=\"%s\">"
        "<tt:Framerate>%d</tt:Framerate>"
        "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>"
        "</trt:VideoSources>"
        "</trt:GetVideoSourcesResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        token, frame_rate, width, height);
}

static int encoder_configuration_body(char *buf, size_t n, const char *token, int width, int height,
                                      int frame_rate, int bitrate_kbps)
{
    return snprintf(buf, n,
                    "<trt:VideoEncoderConfiguration token=\"%s\">"
                    "<tt:Name>VideoEncoder_1</tt:Name>"
                    "<tt:UseCount>1</tt:UseCount>"
                    "<tt:Encoding>JPEG</tt:Encoding>"
                    "<tt:Resolution>"
                    "<tt:Width>%d</tt:Width>"
                    "<tt:Height>%d</tt:Height>"
                    "</tt:Resolution>"
                    "<tt:Quality>5</tt:Quality>"
                    "<tt:RateControl>"
                    "<tt:FrameRateLimit>%d</tt:FrameRateLimit>"
                    "<tt:EncodingInterval>1</tt:EncodingInterval>"
                    "<tt:BitrateLimit>%d</tt:BitrateLimit>"
                    "</tt:RateControl>"
                    "</trt:VideoEncoderConfiguration>",
                    token, width, height, frame_rate, bitrate_kbps);
}

int onvif_xml_video_encoder_configurations(char *buf, size_t n, const char *token, int width,
                                           int height, int frame_rate, int bitrate_kbps)
{
    int off = snprintf(buf, n,
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<soap:Envelope"
                       " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                       " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
                       " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
                       "<soap:Body>"
                       "<trt:GetVideoEncoderConfigurationsResponse>");
    if (off < 0 || (size_t)off >= n) {
        return off;
    }
    off += encoder_configuration_body(buf + off, n - off, token, width, height, frame_rate,
                                      bitrate_kbps);
    if (off < 0 || (size_t)off >= n) {
        return off;
    }
    off += snprintf(buf + off, n - off,
                    "</trt:GetVideoEncoderConfigurationsResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>");
    return off;
}

int onvif_xml_video_encoder_configuration(char *buf, size_t n, const char *token, int width,
                                          int height, int frame_rate, int bitrate_kbps)
{
    int off = snprintf(buf, n,
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<soap:Envelope"
                       " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                       " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
                       " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
                       "<soap:Body>"
                       "<trt:GetVideoEncoderConfigurationResponse>");
    if (off < 0 || (size_t)off >= n) {
        return off;
    }
    off += encoder_configuration_body(buf + off, n - off, token, width, height, frame_rate,
                                      bitrate_kbps);
    if (off < 0 || (size_t)off >= n) {
        return off;
    }
    off += snprintf(buf + off, n - off,
                    "</trt:GetVideoEncoderConfigurationResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>");
    return off;
}

int onvif_xml_video_encoder_configuration_options(char *buf, size_t n, int width, int height,
                                                  int frame_rate)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
                    " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
                    "<soap:Body>"
                    "<trt:GetVideoEncoderConfigurationOptionsResponse>"
                    "<trt:Options>"
                    "<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>10</tt:Max></tt:QualityRange>"
                    "<tt:JPEG>"
                    "<tt:ResolutionsAvailable>"
                    "<tt:Width>%d</tt:Width><tt:Height>%d</tt:Height>"
                    "</tt:ResolutionsAvailable>"
                    "<tt:FrameRateRange>"
                    "<tt:Min>1</tt:Min><tt:Max>%d</tt:Max>"
                    "</tt:FrameRateRange>"
                    "<tt:EncodingIntervalRange>"
                    "<tt:Min>1</tt:Min><tt:Max>1</tt:Max>"
                    "</tt:EncodingIntervalRange>"
                    "</tt:JPEG>"
                    "</trt:Options>"
                    "</trt:GetVideoEncoderConfigurationOptionsResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    width, height, frame_rate);
}

int onvif_xml_set_video_encoder_configuration_ack(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
                    "<soap:Body>"
                    "<trt:SetVideoEncoderConfigurationResponse/>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}

int onvif_xml_guaranteed_encoder_instances(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
                    "<soap:Body>"
                    "<trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse>"
                    "<trt:TotalInstances>1</trt:TotalInstances>"
                    "</trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}

int onvif_xml_set_synchronization_point_ack(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
                    "<soap:Body>"
                    "<trt:SetSynchronizationPointResponse/>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}

int onvif_xml_media_service_capabilities(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
                    "<soap:Body>"
                    "<trt:GetServiceCapabilitiesResponse>"
                    "<trt:Capabilities SnapshotUri=\"true\" RTP_US=\"false\" "
                    "RTP_Multicast=\"false\" RTP_TCP=\"true\" NonFixedIP=\"false\"/>"
                    "</trt:GetServiceCapabilitiesResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}

/* ------------------------------------------------------------------ */
/*  Events service statics (issue #15)                                 */
/* ------------------------------------------------------------------ */

int onvif_xml_event_properties(char *buf, size_t n)
{
    return snprintf(
        buf, n,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope"
        " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
        " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">"
        "<s:Body>"
        "<tev:GetEventPropertiesResponse>"
        "<tev:TopicNamespaceLocation>http://www.onvif.org/ver10/topics</tev:TopicNamespaceLocation>"
        "<wsnt:FixedTopicSet>true</wsnt:FixedTopicSet>"
        "<tev:TopicExpressionDialect>http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet</"
        "tev:TopicExpressionDialect>"
        "<tev:MessageContentFilterDialect>http://www.onvif.org/ver10/tev/messageContentFilter/"
        "ItemFilter</tev:MessageContentFilterDialect>"
        "</tev:GetEventPropertiesResponse>"
        "</s:Body>"
        "</s:Envelope>");
}

int onvif_xml_events_service_capabilities(char *buf, size_t n)
{
    return snprintf(
        buf, n,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope"
        " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
        "<s:Body>"
        "<tev:GetServiceCapabilitiesResponse>"
        "<tev:Capabilities WSSubscriptionPolicySupport=\"false\" WSPullPointSupport=\"true\" "
        "WSPausableSubscriptionManagerInterfaceSupport=\"false\"/>"
        "</tev:GetServiceCapabilitiesResponse>"
        "</s:Body>"
        "</s:Envelope>");
}

int onvif_xml_events_sync_point_ack(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<s:Envelope"
                    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
                    "<s:Body>"
                    "<tev:SetSynchronizationPointResponse/>"
                    "</s:Body>"
                    "</s:Envelope>");
}

int onvif_xml_fault_not_authorized(char *buf, size_t n)
{
    return snprintf(
        buf, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<soap:Body>"
        "<soap:Fault>"
        "<soap:Code>"
        "<soap:Value>soap:Sender</soap:Value>"
        "<soap:Subcode>"
        "<soap:Value>tds:NotAuthorized</soap:Value>"
        "</soap:Subcode>"
        "</soap:Code>"
        "<soap:Reason><soap:Text xml:lang=\"en\">Sender not authorized</soap:Text></soap:Reason>"
        "</soap:Fault>"
        "</soap:Body>"
        "</soap:Envelope>");
}

/* ------------------------------------------------------------------ */
/*  Media2 service — minimal Profile-T subset (issue #18)              */
/*  Namespace family switches to tr2 (ver20/media); the Media1 byte    */
/*  set is untouched.                                                  */
/* ------------------------------------------------------------------ */

int onvif_xml_media2_profiles(char *buf, size_t n, int frame_rate)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
                    " xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\">"
                    "<soap:Body>"
                    "<tr2:GetProfilesResponse>"
                    "<tr2:Profiles token=\"MainStream\" fixed=\"true\">"
                    "<tt:Name>MainStream</tt:Name>"
                    "<tr2:Configurations>"
                    "<tr2:VideoSource token=\"VideoSource_1\">"
                    "<tt:Name>VideoSource_1</tt:Name>"
                    "<tt:UseCount>1</tt:UseCount>"
                    "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
                    "</tr2:VideoSource>"
                    "<tr2:VideoEncoder token=\"VideoEncoder_1\">"
                    "<tt:Name>VideoEncoder_1</tt:Name>"
                    "<tt:UseCount>1</tt:UseCount>"
                    "<tt:Encoding>JPEG</tt:Encoding>"
                    "<tt:Resolution>"
                    "<tt:Width>640</tt:Width>"
                    "<tt:Height>480</tt:Height>"
                    "</tt:Resolution>"
                    "<tt:RateControl>"
                    "<tt:FrameRateLimit>%d</tt:FrameRateLimit>"
                    "<tt:BitrateLimit>4096</tt:BitrateLimit>"
                    "</tt:RateControl>"
                    "</tr2:VideoEncoder>"
                    "</tr2:Configurations>"
                    "</tr2:Profiles>"
                    "</tr2:GetProfilesResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    frame_rate);
}

int onvif_xml_media2_stream_uri(char *buf, size_t n, const char *uri)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\">"
                    "<soap:Body>"
                    "<tr2:GetStreamUriResponse>"
                    "<tr2:Uri>%s</tr2:Uri>"
                    "</tr2:GetStreamUriResponse>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    uri);
}

int onvif_xml_media2_sync_point_ack(char *buf, size_t n)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\">"
                    "<soap:Body>"
                    "<tr2:SetSynchronizationPointResponse/>"
                    "</soap:Body>"
                    "</soap:Envelope>");
}
