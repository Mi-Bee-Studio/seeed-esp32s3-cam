/*
 * onvif-c core — WS-Discovery Probe handling and response builders.
 *
 * Extracted from the MiBee Cam firmware; response bytes pinned by golden
 * tests. Scopes is injected so integrators can advertise their own
 * hardware/name strings.
 */

#include "onvif_probe.h"

#include <stdio.h>
#include <string.h>

bool onvif_probe_is_probe(const char *body)
{
    if ((strstr(body, "wsdiscovery:Probe") || strstr(body, "ws-discovery:Probe") ||
         strstr(body, ":Probe")) &&
        !strstr(body, "ProbeMatches")) {
        return true;
    }
    return false;
}

bool onvif_probe_message_id(const char *body, char *out, size_t n)
{
    const char *tag = strstr(body, "MessageID");
    if (!tag) {
        return false;
    }

    const char *gt = strchr(tag, '>');
    if (!gt) {
        return false;
    }
    gt++;

    const char *lt = strchr(gt, '<');
    if (!lt) {
        return false;
    }

    size_t len = (size_t)(lt - gt);
    if (len == 0 || len >= n) {
        return false;
    }

    memcpy(out, gt, len);
    out[len] = '\0';
    return true;
}

static int build_matches_family(char *buf, size_t n, const char *outer, const char *inner,
                                const char *relates_to, const char *uuid, const char *ip,
                                unsigned port, const char *scopes);

int onvif_probe_build_matches(char *buf, size_t n, const char *relates_to, const char *uuid,
                              const char *ip, unsigned port, const char *scopes)
{
    return build_matches_family(buf, n, "ProbeMatches", "ProbeMatch", relates_to, uuid, ip, port,
                                scopes);
}

int onvif_probe_build_resolve_matches(char *buf, size_t n, const char *relates_to, const char *uuid,
                                      const char *ip, unsigned port)
{
    /* ResolveMatches carries XAddrs but no Types/Scopes per the WSDL. */
    return build_matches_family(buf, n, "ResolveMatches", "ResolveMatch", relates_to, uuid, ip,
                                port, "");
}

static int build_matches_family(char *buf, size_t n, const char *outer, const char *inner,
                                const char *relates_to, const char *uuid, const char *ip,
                                unsigned port, const char *scopes)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
                    " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
                    " xmlns:wsdp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\">"
                    "<soap:Header>"
                    "<wsa:Action>"
                    "http://schemas.xmlsoap.org/ws/2005/04/discovery/%s"
                    "</wsa:Action>"
                    "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
                    "<wsa:RelatesTo>%s</wsa:RelatesTo>"
                    "<wsa:To>"
                    "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
                    "</wsa:To>"
                    "<wsd:AppSequence InstanceId=\"14200\" MessageNumber=\"1\"/>"
                    "</soap:Header>"
                    "<soap:Body>"
                    "<wsd:%s>"
                    "<wsd:%s>"
                    "<wsa:EndpointReference>"
                    "<wsa:Address>urn:uuid:%s</wsa:Address>"
                    "</wsa:EndpointReference>"
                    "%s%s%s%s"
                    "<wsd:XAddrs>http://%s:%u/onvif/device_service</wsd:XAddrs>"
                    "<wsd:MetadataVersion>2</wsd:MetadataVersion>"
                    "</wsd:%s>"
                    "</wsd:%s>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    outer, uuid, relates_to ? relates_to : "", outer, inner, uuid,
                    scopes && *scopes ? "<wsd:Types>tns:NetworkVideoTransmitter</wsd:Types>" : "",
                    scopes && *scopes ? "<wsd:Scopes>" : "", scopes && *scopes ? scopes : "",
                    scopes && *scopes ? "</wsd:Scopes>" : "", ip, port, inner, outer);
}

int onvif_probe_build_hello(char *buf, size_t n, const char *uuid, const char *ip, unsigned port,
                            const char *scopes)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
                    " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
                    " xmlns:wsdp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\">"
                    "<soap:Header>"
                    "<wsa:Action>"
                    "http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello"
                    "</wsa:Action>"
                    "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
                    "<wsa:To>"
                    "urn:schemas-xmlsoap-org:ws:2005:04:discovery"
                    "</wsa:To>"
                    "<wsd:AppSequence InstanceId=\"14200\" MessageNumber=\"1\"/>"
                    "</soap:Header>"
                    "<soap:Body>"
                    "<wsd:Hello>"
                    "<wsa:EndpointReference>"
                    "<wsa:Address>urn:uuid:%s</wsa:Address>"
                    "</wsa:EndpointReference>"
                    "<wsd:Types>tns:NetworkVideoTransmitter</wsd:Types>"
                    "<wsd:Scopes>%s</wsd:Scopes>"
                    "<wsd:XAddrs>http://%s:%u/onvif/device_service</wsd:XAddrs>"
                    "<wsd:MetadataVersion>2</wsd:MetadataVersion>"
                    "</wsd:Hello>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    uuid, uuid, scopes, ip, port);
}

bool onvif_probe_is_resolve(const char *body)
{
    if (strstr(body, ":Resolve") && !strstr(body, "ResolveMatches")) {
        return true;
    }
    return false;
}

int onvif_probe_build_bye(char *buf, size_t n, const char *uuid)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<soap:Envelope"
                    " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                    " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
                    " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
                    "<soap:Header>"
                    "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
                    "<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
                    "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Bye</wsa:Action>"
                    "<wsd:AppSequence InstanceId=\"1\" MessageNumber=\"2\"/>"
                    "</soap:Header>"
                    "<soap:Body>"
                    "<wsd:Bye>"
                    "<wsa:EndpointReference>"
                    "<wsa:Address>urn:uuid:%s</wsa:Address>"
                    "</wsa:EndpointReference>"
                    "</wsd:Bye>"
                    "</soap:Body>"
                    "</soap:Envelope>",
                    uuid, uuid);
}
