/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "gtp-path.h"
#include "pfcp-path.h"

#include "s5c-handler.h"

static void bearer_timeout(ogs_gtp_xact_t *xact, void *data)
{
    sgwc_bearer_t *bearer = data;
    sgwc_sess_t *sess = NULL;
    sgwc_ue_t *sgwc_ue = NULL;
    uint8_t type = 0;

    ogs_assert(xact);
    ogs_assert(bearer);
    sess = bearer->sess;
    ogs_assert(sess);
    sgwc_ue = sess->sgwc_ue;
    ogs_assert(sgwc_ue);

    type = xact->seq[0].type;

    switch (type) {
    case OGS_GTP_UPDATE_BEARER_REQUEST_TYPE:
        ogs_error("[%s] No Update Bearer Response", sgwc_ue->imsi_bcd);
        break;
    case OGS_GTP_DELETE_BEARER_REQUEST_TYPE:
        ogs_error("[%s] No Delete Bearer Response", sgwc_ue->imsi_bcd);
        if (!sgwc_bearer_cycle(bearer)) {
            ogs_warn("[%s] Bearer has already been removed", sgwc_ue->imsi_bcd);
            break;
        }
        ogs_assert(OGS_OK ==
            sgwc_pfcp_send_bearer_modification_request(
                bearer, NULL, NULL, OGS_PFCP_MODIFY_REMOVE));
        break;
    default:
        ogs_error("GTP Timeout : IMSI[%s] Message-Type[%d]",
                sgwc_ue->imsi_bcd, type);
    }
}

void sgwc_s5c_handle_create_session_response(
        sgwc_sess_t *sess, ogs_gtp_xact_t *s5c_xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_message_t *message)
{
    int rv;
    uint8_t cause_value;

    sgwc_ue_t *sgwc_ue = NULL;
    sgwc_bearer_t *bearer = NULL;
    sgwc_tunnel_t *ul_tunnel = NULL;
    ogs_pfcp_far_t *far = NULL;

    ogs_gtp_f_teid_t *pgw_s5c_teid = NULL;
    ogs_gtp_f_teid_t *pgw_s5u_teid = NULL;

    ogs_gtp_create_session_response_t *rsp = NULL;
    ogs_gtp_xact_t *s11_xact = NULL;

    ogs_assert(s5c_xact);
    s11_xact = s5c_xact->assoc_xact;
    ogs_assert(s11_xact);
    ogs_assert(gtpbuf);
    ogs_assert(message);
    rsp = &message->create_session_response;
    ogs_assert(rsp);

    ogs_debug("Create Session Response");

    cause_value = OGS_GTP_CAUSE_REQUEST_ACCEPTED;

    rv = ogs_gtp_xact_commit(s5c_xact);
    ogs_expect(rv == OGS_OK);

    if (rsp->bearer_contexts_created.cause.presence == 0) {
        ogs_error("No EPS Bearer Cause");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (rsp->bearer_contexts_created.presence == 0) {
        ogs_error("No Bearer");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (rsp->bearer_contexts_created.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (rsp->pgw_s5_s8__s2a_s2b_f_teid_for_pmip_based_interface_or_for_gtp_based_control_plane_interface.
            presence == 0) {
        ogs_error("No GTP TEID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (rsp->bearer_contexts_created.s5_s8_u_sgw_f_teid.presence == 0) {
        ogs_error("No GTP TEID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (rsp->pdn_address_allocation.presence) {
        ogs_paa_t paa;

        memcpy(&paa, rsp->pdn_address_allocation.data,
                ogs_min(sizeof(paa), rsp->pdn_address_allocation.len));
        if (paa.session_type == OGS_PDU_SESSION_TYPE_IPV4) {
            /* Nothing */
        } else if (paa.session_type == OGS_PDU_SESSION_TYPE_IPV6) {
            /* Nothing */
        } else if (paa.session_type == OGS_PDU_SESSION_TYPE_IPV4V6) {
            /* Nothing */
        } else {
            ogs_error("Unknown PDN Type %u", paa.session_type);
            cause_value = OGS_GTP_CAUSE_MANDATORY_IE_INCORRECT;
        }

    } else {
        ogs_error("No PDN Address Allocation");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (!sess) {
        ogs_warn("No Context in TEID");
        sess = s5c_xact->data;
        ogs_assert(sess);
    }

    sgwc_ue = sess->sgwc_ue;
    ogs_assert(sgwc_ue);

    if (cause_value == OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        bearer = sgwc_bearer_find_by_sess_ebi(sess,
                    rsp->bearer_contexts_created.eps_bearer_id.u8);
        if (!bearer)
            ogs_error("No Context for EPS Bearer ID[%d]",
                    rsp->bearer_contexts_created.eps_bearer_id.u8);
    }
    if (!bearer) {
        ogs_warn("No Context");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (rsp->cause.presence) {
        ogs_gtp_cause_t *cause = rsp->cause.data;
        ogs_assert(cause);

        cause_value = cause->value;
        if (cause_value == OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
            if (rsp->bearer_contexts_created.cause.presence) {
                cause = rsp->bearer_contexts_created.cause.data;
                ogs_assert(cause);

                cause_value = cause->value;
            } else {
                ogs_error("No Cause");
                cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
            }
        } else {
            ogs_warn("GTP Failed [CAUSE:%d]", cause_value);
        }
    } else {
        ogs_error("No Cause");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cause_value != OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp_send_error_message(
                s11_xact, sgwc_ue ? sgwc_ue->mme_s11_teid : 0,
                OGS_GTP_CREATE_SESSION_RESPONSE_TYPE, cause_value);
        return;
    }

    ogs_assert(bearer);
    ul_tunnel = sgwc_ul_tunnel_in_bearer(bearer);
    ogs_assert(ul_tunnel);

    /* Receive Data Plane(UL) : PGW-S5U */
    pgw_s5u_teid = rsp->bearer_contexts_created.s5_s8_u_sgw_f_teid.data;
    ogs_assert(pgw_s5u_teid);
    ul_tunnel->remote_teid = be32toh(pgw_s5u_teid->teid);

    rv = ogs_gtp_f_teid_to_ip(pgw_s5u_teid, &ul_tunnel->remote_ip);
    if (rv != OGS_OK) {
        ogs_gtp_send_error_message(
                s11_xact, sgwc_ue ? sgwc_ue->mme_s11_teid : 0,
                OGS_GTP_CREATE_SESSION_RESPONSE_TYPE,
                OGS_GTP_CAUSE_MANDATORY_IE_MISSING);
        return;
    }

    far = ul_tunnel->far;
    ogs_assert(far);

    far->apply_action = OGS_PFCP_APPLY_ACTION_FORW;

    ogs_assert(OGS_OK ==
        ogs_pfcp_ip_to_outer_header_creation(&ul_tunnel->remote_ip,
            &far->outer_header_creation, &far->outer_header_creation_len));
    far->outer_header_creation.teid = ul_tunnel->remote_teid;

    /* Receive Control Plane(UL) : PGW-S5C */
    pgw_s5c_teid = rsp->pgw_s5_s8__s2a_s2b_f_teid_for_pmip_based_interface_or_for_gtp_based_control_plane_interface.data;
    ogs_assert(pgw_s5c_teid);
    sess->pgw_s5c_teid = be32toh(pgw_s5c_teid->teid);

    ogs_debug("    MME_S11_TEID[%d] SGW_S11_TEID[%d]",
        sgwc_ue->mme_s11_teid, sgwc_ue->sgw_s11_teid);
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
        sess->sgw_s5c_teid, sess->pgw_s5c_teid);
    ogs_debug("    SGW_S5U_TEID[%d] PGW_S5U_TEID[%d]",
        ul_tunnel->local_teid, ul_tunnel->remote_teid);

    ogs_assert(OGS_OK ==
        sgwc_pfcp_send_bearer_modification_request(
            bearer, s11_xact, gtpbuf,
            OGS_PFCP_MODIFY_UL_ONLY|OGS_PFCP_MODIFY_ACTIVATE));
}

void sgwc_s5c_handle_delete_session_response(
        sgwc_sess_t *sess, ogs_gtp_xact_t *s5c_xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_message_t *message)
{
    int rv;
    uint8_t cause_value;

    sgwc_ue_t *sgwc_ue = NULL;

    ogs_gtp_xact_t *s11_xact = NULL;
    ogs_gtp_delete_session_response_t *rsp = NULL;

    ogs_assert(s5c_xact);
    s11_xact = s5c_xact->assoc_xact;
    ogs_assert(s11_xact);
    ogs_assert(message);
    rsp = &message->delete_session_response;
    ogs_assert(rsp);

    ogs_debug("Delete Session Response");

    cause_value = OGS_GTP_CAUSE_REQUEST_ACCEPTED;

    rv = ogs_gtp_xact_commit(s5c_xact);
    ogs_expect(rv == OGS_OK);

    if (!sess) {
        ogs_warn("No Context in TEID");
        sess = s5c_xact->data;
        ogs_assert(sess);
    }

    sgwc_ue = sess->sgwc_ue;
    ogs_assert(sgwc_ue);

    if (rsp->cause.presence) {
        ogs_gtp_cause_t *cause = rsp->cause.data;
        ogs_assert(cause);

        cause_value = cause->value;
    } else {
        ogs_error("No Cause");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cause_value != OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        ogs_assert(OGS_OK ==
            sgwc_pfcp_send_session_deletion_request(sess, NULL, NULL));
        ogs_gtp_send_error_message(
                s11_xact, sgwc_ue ? sgwc_ue->mme_s11_teid : 0,
                OGS_GTP_DELETE_SESSION_RESPONSE_TYPE, cause_value);
        return;
    }

    /* Remove a pgw session */
    ogs_debug("    MME_S11_TEID[%d] SGW_S11_TEID[%d]",
        sgwc_ue->mme_s11_teid, sgwc_ue->sgw_s11_teid);
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
        sess->sgw_s5c_teid, sess->pgw_s5c_teid);

    /*
     * 1. MME sends Delete Session Request to SGW/SMF.
     * 2. SMF sends Delete Session Response to SGW/MME.
     */
    ogs_assert(OGS_OK ==
        sgwc_pfcp_send_session_deletion_request(sess, s11_xact, gtpbuf));
}

void sgwc_s5c_handle_modify_bearer_response(
        sgwc_sess_t *sess, ogs_gtp_xact_t *s5c_xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_message_t *message)
{
    int rv;
    uint8_t cause_value;

    sgwc_ue_t *sgwc_ue = NULL;
    ogs_pkbuf_t *pkbuf = NULL;

    ogs_gtp_xact_t *s11_xact = NULL;
    ogs_gtp_modify_bearer_response_t *rsp = NULL;

    ogs_assert(s5c_xact);
    s11_xact = s5c_xact->assoc_xact;
    ogs_assert(s11_xact);
    ogs_assert(message);
    rsp = &message->modify_bearer_response;
    ogs_assert(rsp);

    ogs_debug("Modify Bearer Response");

    cause_value = OGS_GTP_CAUSE_REQUEST_ACCEPTED;

    rv = ogs_gtp_xact_commit(s5c_xact);
    ogs_expect(rv == OGS_OK);

    if (!sess) {
        ogs_warn("No Context in TEID");
        sess = s5c_xact->data;
        ogs_assert(sess);
    }

    sgwc_ue = sess->sgwc_ue;
    ogs_assert(sgwc_ue);

    if (rsp->cause.presence) {
        ogs_gtp_cause_t *cause = rsp->cause.data;
        ogs_assert(cause);

        cause_value = cause->value;
    } else {
        ogs_error("No Cause");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cause_value != OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp_send_error_message(
                s11_xact, sgwc_ue ? sgwc_ue->mme_s11_teid : 0,
                OGS_GTP_MODIFY_BEARER_RESPONSE_TYPE, cause_value);
        return;
    }

    ogs_debug("    MME_S11_TEID[%d] SGW_S11_TEID[%d]",
        sgwc_ue->mme_s11_teid, sgwc_ue->sgw_s11_teid);
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
        sess->sgw_s5c_teid, sess->pgw_s5c_teid);

    message->h.type = OGS_GTP_MODIFY_BEARER_RESPONSE_TYPE;
    message->h.teid = sgwc_ue->mme_s11_teid;

    pkbuf = ogs_gtp_build_msg(message);
    ogs_expect_or_return(pkbuf);

    rv = ogs_gtp_xact_update_tx(s11_xact, &message->h, pkbuf);
    ogs_expect_or_return(rv == OGS_OK);

    rv = ogs_gtp_xact_commit(s11_xact);
    ogs_expect(rv == OGS_OK);
}

void sgwc_s5c_handle_create_bearer_request(
        sgwc_sess_t *sess, ogs_gtp_xact_t *s5c_xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_message_t *message)
{
    int rv;
    uint8_t cause_value = 0;

    sgwc_ue_t *sgwc_ue = NULL;
    sgwc_bearer_t *bearer = NULL;
    sgwc_tunnel_t *ul_tunnel = NULL;
    ogs_pfcp_far_t *far = NULL;

    ogs_gtp_create_bearer_request_t *req = NULL;
    ogs_gtp_f_teid_t *pgw_s5u_teid = NULL;

    ogs_assert(s5c_xact);
    ogs_assert(message);
    req = &message->create_bearer_request;
    ogs_assert(req);

    ogs_debug("Create Bearer Request");

    cause_value = OGS_GTP_CAUSE_REQUEST_ACCEPTED;

    if (req->linked_eps_bearer_id.presence == 0) {
        ogs_error("No Linked EBI");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts.presence == 0) {
        ogs_error("No Bearer");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts.s5_s8_u_sgw_f_teid.presence == 0) {
        ogs_error("No GTP TEID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (!sess) {
        ogs_warn("No Context");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (cause_value != OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp_send_error_message(s5c_xact, sess ? sess->pgw_s5c_teid : 0,
                OGS_GTP_CREATE_BEARER_RESPONSE_TYPE, cause_value);
        return;
    }

    sgwc_ue = sess->sgwc_ue;
    ogs_assert(sgwc_ue);

    bearer = sgwc_bearer_add(sess);
    ogs_assert(bearer);
    ul_tunnel = sgwc_ul_tunnel_in_bearer(bearer);
    ogs_assert(ul_tunnel);

    /* Receive Data Plane(UL) : PGW-S5U */
    pgw_s5u_teid = req->bearer_contexts.s5_s8_u_sgw_f_teid.data;
    ogs_assert(pgw_s5u_teid);
    ul_tunnel->remote_teid = be32toh(pgw_s5u_teid->teid);

    ogs_debug("    MME_S11_TEID[%d] SGW_S11_TEID[%d]",
        sgwc_ue->mme_s11_teid, sgwc_ue->sgw_s11_teid);
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
        sess->sgw_s5c_teid, sess->pgw_s5c_teid);
    ogs_debug("    SGW_S5U_TEID[%d] PGW_S5U_TEID[%d]",
        ul_tunnel->local_teid, ul_tunnel->remote_teid);

    rv = ogs_gtp_f_teid_to_ip(pgw_s5u_teid, &ul_tunnel->remote_ip);
    if (rv != OGS_OK) {
        ogs_gtp_send_error_message(s5c_xact, sess ? sess->pgw_s5c_teid : 0,
                OGS_GTP_CREATE_BEARER_RESPONSE_TYPE,
                OGS_GTP_CAUSE_MANDATORY_IE_MISSING);
        return;
    }

    far = ul_tunnel->far;
    ogs_assert(far);

    far->apply_action = OGS_PFCP_APPLY_ACTION_FORW;

    ogs_assert(OGS_OK ==
        ogs_pfcp_ip_to_outer_header_creation(&ul_tunnel->remote_ip,
            &far->outer_header_creation, &far->outer_header_creation_len));
    far->outer_header_creation.teid = ul_tunnel->remote_teid;

    ogs_assert(OGS_OK ==
        sgwc_pfcp_send_bearer_modification_request(
            bearer, s5c_xact, gtpbuf,
            OGS_PFCP_MODIFY_UL_ONLY|OGS_PFCP_MODIFY_CREATE));
}

void sgwc_s5c_handle_update_bearer_request(
        sgwc_sess_t *sess, ogs_gtp_xact_t *s5c_xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_message_t *message)
{
    int rv;
    uint8_t cause_value = 0;
    ogs_gtp_xact_t *s11_xact = NULL;
    ogs_gtp_update_bearer_request_t *req = NULL;
    ogs_pkbuf_t *pkbuf = NULL;
    sgwc_ue_t *sgwc_ue = NULL;
    sgwc_bearer_t *bearer = NULL;

    ogs_assert(s5c_xact);
    ogs_assert(message);
    req = &message->update_bearer_request;
    ogs_assert(req);

    ogs_debug("Update Bearer Request");

    cause_value = OGS_GTP_CAUSE_REQUEST_ACCEPTED;

    if (req->bearer_contexts.presence == 0) {
        ogs_error("No Bearer");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (!sess) {
        ogs_warn("No Context");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (cause_value == OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        bearer = sgwc_bearer_find_by_sess_ebi(
                sess, req->bearer_contexts.eps_bearer_id.u8);
        if (!bearer)
            ogs_error("No Context for EPS Bearer ID[%d]",
                        req->bearer_contexts.eps_bearer_id.u8);
    }
    if (!bearer) {
        ogs_warn("No Context");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (cause_value != OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp_send_error_message(s5c_xact, sess ? sess->pgw_s5c_teid : 0,
                OGS_GTP_UPDATE_BEARER_RESPONSE_TYPE, cause_value);
        return;
    }

    ogs_assert(sess);
    ogs_assert(bearer);
    sgwc_ue = sess->sgwc_ue;
    ogs_assert(sgwc_ue);
    ogs_assert(sgwc_ue->gnode);

    ogs_debug("    EBI[%d]", bearer->ebi);
    ogs_debug("    MME_S11_TEID[%d] SGW_S11_TEID[%d]",
        sgwc_ue->mme_s11_teid, sgwc_ue->sgw_s11_teid);
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
        sess->sgw_s5c_teid, sess->pgw_s5c_teid);

    message->h.type = OGS_GTP_UPDATE_BEARER_REQUEST_TYPE;
    message->h.teid = sgwc_ue->mme_s11_teid;

    pkbuf = ogs_gtp_build_msg(message);
    ogs_expect_or_return(pkbuf);

    s11_xact = s5c_xact->assoc_xact;
    if (!s11_xact) {
        s11_xact = ogs_gtp_xact_local_create(
                sgwc_ue->gnode, &message->h, pkbuf, bearer_timeout, bearer);
        ogs_expect_or_return(s11_xact);

        ogs_gtp_xact_associate(s5c_xact, s11_xact);
    } else {
        rv = ogs_gtp_xact_update_tx(s11_xact, &message->h, pkbuf);
        ogs_expect_or_return(rv == OGS_OK);
    }

    rv = ogs_gtp_xact_commit(s11_xact);
    ogs_expect(rv == OGS_OK);

    ogs_debug("Update Bearer Request : SGW <-- PGW");
}

void sgwc_s5c_handle_delete_bearer_request(
        sgwc_sess_t *sess, ogs_gtp_xact_t *s5c_xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_message_t *message)
{
    int rv;
    uint8_t cause_value = 0;
    ogs_gtp_xact_t *s11_xact = NULL;
    ogs_gtp_delete_bearer_request_t *req = NULL;
    ogs_pkbuf_t *pkbuf = NULL;
    sgwc_ue_t *sgwc_ue = NULL;
    sgwc_bearer_t *bearer = NULL;

    ogs_assert(s5c_xact);
    ogs_assert(message);
    req = &message->delete_bearer_request;
    ogs_assert(req);

    ogs_debug("Delete Bearer Request");

    cause_value = OGS_GTP_CAUSE_REQUEST_ACCEPTED;

    if (req->linked_eps_bearer_id.presence == 0 &&
        req->eps_bearer_ids.presence == 0) {
        ogs_error("No Linked EBI or EPS Bearer ID");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (!sess) {
        ogs_warn("No Context");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (cause_value == OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        uint8_t ebi;

        if (req->linked_eps_bearer_id.presence) {
           /*
            * << Linked EPS Bearer ID >>
            *
            * 1. SMF sends Delete Bearer Request(DEFAULT BEARER) to SGW/MME.
            * 2. MME sends Delete Bearer Response to SGW/SMF.
            *
            * OR
            *
            * 1. SMF sends Delete Bearer Request(DEFAULT BEARER) to ePDG.
            * 2. ePDG sends Delete Bearer Response(DEFAULT BEARER) to SMF.
            */
            ebi = req->linked_eps_bearer_id.u8;
        } else if (req->eps_bearer_ids.presence) {
           /*
            * << EPS Bearer IDs >>
            *
            * 1. MME sends Bearer Resource Command to SGW/SMF.
            * 2. SMF sends Delete Bearer Request(DEDICATED BEARER) to SGW/MME.
            * 3. MME sends Delete Bearer Response(DEDICATED BEARER) to SGW/SMF.
            *
            * OR
            *
            * 1. SMF sends Delete Bearer Request(DEDICATED BEARER) to SGW/MME.
            * 2. MME sends Delete Bearer Response(DEDICATED BEARER) to SGW/SMF.
            */
            ebi = req->eps_bearer_ids.u8;
        } else
            ogs_assert_if_reached();

        bearer = sgwc_bearer_find_by_sess_ebi(sess, ebi);
        if (!bearer)
            ogs_error("No Context for EPS Bearer ID[%d]", ebi);
    }
    if (!bearer) {
        ogs_warn("No Context");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (cause_value != OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp_send_error_message(s5c_xact, sess ? sess->pgw_s5c_teid : 0,
                OGS_GTP_DELETE_BEARER_RESPONSE_TYPE, cause_value);
        return;
    }

    ogs_assert(sess);
    ogs_assert(bearer);
    sgwc_ue = sess->sgwc_ue;
    ogs_assert(sgwc_ue);
    ogs_assert(sgwc_ue->gnode);

    ogs_debug("    EBI[%d]", bearer->ebi);
    ogs_debug("    MME_S11_TEID[%d] SGW_S11_TEID[%d]",
        sgwc_ue->mme_s11_teid, sgwc_ue->sgw_s11_teid);
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
        sess->sgw_s5c_teid, sess->pgw_s5c_teid);

    message->h.type = OGS_GTP_DELETE_BEARER_REQUEST_TYPE;
    message->h.teid = sgwc_ue->mme_s11_teid;

    pkbuf = ogs_gtp_build_msg(message);
    ogs_expect_or_return(pkbuf);

    s11_xact = s5c_xact->assoc_xact;
    if (!s11_xact) {
       /*
        * 1. SMF sends Delete Bearer Request(DEFAULT BEARER) to SGW/MME.
        * 2. MME sends Delete Bearer Response to SGW/SMF.
        *
        * OR
        *
        * 1. SMF sends Delete Bearer Request(DEFAULT BEARER) to ePDG.
        * 2. ePDG sends Delete Bearer Response(DEFAULT BEARER) to SMF.
        *
        * OR
        *
        * 1. SMF sends Delete Bearer Request(DEDICATED BEARER) to SGW/MME.
        * 2. MME sends Delete Bearer Response(DEDICATED BEARER) to SGW/SMF.
        */
        s11_xact = ogs_gtp_xact_local_create(
                sgwc_ue->gnode, &message->h, pkbuf, bearer_timeout, bearer);
        ogs_expect_or_return(s11_xact);

        ogs_gtp_xact_associate(s5c_xact, s11_xact);
    } else {
       /*
        * 1. MME sends Bearer Resource Command to SGW/SMF.
        * 2. SMF sends Delete Bearer Request(DEDICATED BEARER) to SGW/MME.
        * 3. MME sends Delete Bearer Response(DEDICATED BEARER) to SGW/SMF.
        */
        rv = ogs_gtp_xact_update_tx(s11_xact, &message->h, pkbuf);
        ogs_expect_or_return(rv == OGS_OK);
    }

    rv = ogs_gtp_xact_commit(s11_xact);
    ogs_expect(rv == OGS_OK);
}

void sgwc_s5c_handle_bearer_resource_failure_indication(
        sgwc_sess_t *sess, ogs_gtp_xact_t *s5c_xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_message_t *message)
{
    uint8_t cause_value = 0;
    ogs_gtp_xact_t *s11_xact = NULL;
    ogs_gtp_bearer_resource_failure_indication_t *ind = NULL;

    sgwc_ue_t *sgwc_ue = NULL;

    ogs_assert(s5c_xact);
    s11_xact = s5c_xact->assoc_xact;
    ogs_assert(s11_xact);
    ogs_assert(message);
    ind = &message->bearer_resource_failure_indication;
    ogs_assert(ind);

    ogs_debug("Bearer Resource Failure Indication");

    if (!sess) {
        ogs_warn("No Context");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (ind->cause.presence) {
        ogs_gtp_cause_t *cause = ind->cause.data;
        ogs_assert(cause);

        cause_value = cause->value;
    } else {
        ogs_error("No Cause");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    sgwc_ue = sess->sgwc_ue;
    ogs_assert(sgwc_ue);

    ogs_gtp_send_error_message(s11_xact, sgwc_ue ? sgwc_ue->mme_s11_teid : 0,
            OGS_GTP_BEARER_RESOURCE_FAILURE_INDICATION_TYPE, cause_value);
}
