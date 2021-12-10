/**
 *  @file GasKinetics.cpp Homogeneous kinetics in ideal gases
 */

// This file is part of Cantera. See License.txt in the top-level directory or
// at https://cantera.org/license.txt for license and copyright information.

#include "cantera/kinetics/GasKinetics.h"
#include "cantera/thermo/ThermoPhase.h"

using namespace std;

namespace Cantera
{
GasKinetics::GasKinetics(ThermoPhase* thermo) :
    BulkKinetics(thermo),
    m_logStandConc(0.0),
    m_pres(0.0)
{
    setJacobianSettings(AnyMap()); // use default settings
}

void GasKinetics::resizeReactions()
{
    m_rbuf0.resize(nReactions());
    m_rbuf1.resize(nReactions());
    m_rbuf2.resize(nReactions());

    BulkKinetics::resizeReactions();
}

void GasKinetics::getThirdBodyConcentrations(double* concm)
{
    updateROP();
    std::copy(m_concm.begin(), m_concm.end(), concm);
}

void GasKinetics::update_rates_T()
{
    double T = thermo().temperature();
    double P = thermo().pressure();
    m_logStandConc = log(thermo().standardConcentration());
    double logT = log(T);

    if (T != m_temp) {
        // Update forward rate constant for each reaction
        if (!m_rfn.empty()) {
            m_rates.update(T, logT, m_rfn.data());
        }

        // Falloff reactions (legacy)
        if (!m_rfn_low.empty()) {
            m_falloff_low_rates.update(T, logT, m_rfn_low.data());
            m_falloff_high_rates.update(T, logT, m_rfn_high.data());
        }
        if (!falloff_work.empty()) {
            m_falloffn.updateTemp(T, falloff_work.data());
        }

        updateKc();
        m_ROP_ok = false;
    }

    // loop over MultiBulkRate evaluators for each reaction type
    for (auto& rates : m_bulk_rates) {
        bool changed = rates->update(thermo(), *this);
        if (changed) {
            rates->getRateConstants(m_rfn.data());
            m_ROP_ok = false;
        }
    }
    if (T != m_temp || P != m_pres) {
        // P-log reactions (legacy)
        if (m_plog_rates.nReactions()) {
            m_plog_rates.update(T, logT, m_rfn.data());
            m_ROP_ok = false;
        }

        // Chebyshev reactions (legacy)
        if (m_cheb_rates.nReactions()) {
            m_cheb_rates.update(T, logT, m_rfn.data());
            m_ROP_ok = false;
        }
    }
    m_pres = P;
    m_temp = T;
}

void GasKinetics::update_rates_C()
{
    thermo().getActivityConcentrations(m_act_conc.data());
    thermo().getConcentrations(m_phys_conc.data());
    doublereal ctot = thermo().molarDensity();

    // Third-body objects interacting with MultiRate evaluator
    m_multi_concm.update(m_phys_conc, ctot, m_concm.data());

    // 3-body reactions (legacy)
    if (!concm_3b_values.empty()) {
        m_3b_concm.update(m_phys_conc, ctot, concm_3b_values.data());
        m_3b_concm.copy(concm_3b_values, m_concm.data());
    }

    // Falloff reactions (legacy)
    if (!concm_falloff_values.empty()) {
        m_falloff_concm.update(m_phys_conc, ctot, concm_falloff_values.data());
        m_falloff_concm.copy(concm_falloff_values, m_concm.data());
    }

    // P-log reactions (legacy)
    if (m_plog_rates.nReactions()) {
        double logP = log(thermo().pressure());
        m_plog_rates.update_C(&logP);
    }

    // Chebyshev reactions (legacy)
    if (m_cheb_rates.nReactions()) {
        double log10P = log10(thermo().pressure());
        m_cheb_rates.update_C(&log10P);
    }

    m_ROP_ok = false;
}

void GasKinetics::updateKc()
{
    thermo().getStandardChemPotentials(m_grt.data());
    fill(m_rkcn.begin(), m_rkcn.end(), 0.0);

    // compute Delta G^0 for all reversible reactions
    getRevReactionDelta(m_grt.data(), m_rkcn.data());

    doublereal rrt = 1.0 / thermo().RT();
    for (size_t i = 0; i < m_revindex.size(); i++) {
        size_t irxn = m_revindex[i];
        m_rkcn[irxn] = std::min(exp(m_rkcn[irxn]*rrt - m_dn[irxn]*m_logStandConc),
                                BigNumber);
    }

    for (size_t i = 0; i != m_irrev.size(); ++i) {
        m_rkcn[ m_irrev[i] ] = 0.0;
    }
}

void GasKinetics::processFwdRateCoefficients(double* ropf)
{
    update_rates_C();
    update_rates_T();

    // copy rate coefficients into ropf
    copy(m_rfn.begin(), m_rfn.end(), ropf);

    if (m_falloff_high_rates.nReactions()) {
        processFalloffReactions(ropf);
    }

    // Scale the forward rate coefficient by the perturbation factor
    for (size_t i = 0; i < nReactions(); ++i) {
        ropf[i] *= m_perturb[i];
    }
}

void GasKinetics::processThirdBodies(double* rop)
{
    // multiply rop by enhanced 3b conc for all 3b rxns
    if (!concm_3b_values.empty()) {
        m_3b_concm.multiply(rop, concm_3b_values.data());
    }

    // reactions involving third body
    if (!m_concm.empty()) {
        m_multi_concm.multiply(rop, m_concm.data());
    }
}

void GasKinetics::processEquilibriumConstants(double* rop)
{
    // For reverse rates computed from thermochemistry, multiply the forward
    // rate coefficients by the reciprocals of the equilibrium constants
    for (size_t i = 0; i < nReactions(); ++i) {
        rop[i] *= m_rkcn[i];
    }
}

void GasKinetics::getEquilibriumConstants(doublereal* kc)
{
    update_rates_T();
    thermo().getStandardChemPotentials(m_grt.data());
    fill(m_rkcn.begin(), m_rkcn.end(), 0.0);

    // compute Delta G^0 for all reactions
    getReactionDelta(m_grt.data(), m_rkcn.data());

    doublereal rrt = 1.0 / thermo().RT();
    for (size_t i = 0; i < nReactions(); i++) {
        kc[i] = exp(-m_rkcn[i]*rrt + m_dn[i]*m_logStandConc);
    }

    // force an update of T-dependent properties, so that m_rkcn will
    // be updated before it is used next.
    m_temp = 0.0;
}

void GasKinetics::processFalloffReactions(double* ropf)
{
    // use m_ropr for temporary storage of reduced pressure
    vector_fp& pr = m_ropr;

    for (size_t i = 0; i < m_falloff_low_rates.nReactions(); i++) {
        pr[i] = concm_falloff_values[i] * m_rfn_low[i] / (m_rfn_high[i] + SmallNumber);
        AssertFinite(pr[i], "GasKinetics::processFalloffReactions",
                     "pr[{}] is not finite.", i);
    }

    m_falloffn.pr_to_falloff(pr.data(), falloff_work.data());

    for (size_t i = 0; i < m_falloff_low_rates.nReactions(); i++) {
        if (reactionTypeStr(m_fallindx[i]) == "falloff-legacy") {
            pr[i] *= m_rfn_high[i];
        } else { // CHEMACT_RXN
            pr[i] *= m_rfn_low[i];
        }
        ropf[m_fallindx[i]] = pr[i];
    }
}

void GasKinetics::updateROP()
{
    processFwdRateCoefficients(m_ropf.data());
    processThirdBodies(m_ropf.data());
    copy(m_ropf.begin(), m_ropf.end(), m_ropr.begin());

    // multiply ropf by concentration products
    m_reactantStoich.multiply(m_act_conc.data(), m_ropf.data());

    // for reversible reactions, multiply ropr by concentration products
    processEquilibriumConstants(m_ropr.data());
    m_revProductStoich.multiply(m_act_conc.data(), m_ropr.data());
    for (size_t j = 0; j != nReactions(); ++j) {
        m_ropnet[j] = m_ropf[j] - m_ropr[j];
    }

    for (size_t i = 0; i < m_rfn.size(); i++) {
        AssertFinite(m_rfn[i], "GasKinetics::updateROP",
                     "m_rfn[{}] is not finite.", i);
        AssertFinite(m_ropf[i], "GasKinetics::updateROP",
                     "m_ropf[{}] is not finite.", i);
        AssertFinite(m_ropr[i], "GasKinetics::updateROP",
                     "m_ropr[{}] is not finite.", i);
    }
    m_ROP_ok = true;
}

void GasKinetics::getFwdRateConstants(double* kfwd)
{
    processFwdRateCoefficients(m_ropf.data());

    if (legacy_rate_constants_used()) {
        warn_deprecated("GasKinetics::getFwdRateConstants",
            "Behavior to change after Cantera 2.6;\nresults will no longer include "
            "third-body concentrations for three-body reactions.\nTo switch to new "
            "behavior, use 'cantera.use_legacy_rate_constants(False)' (Python),\n"
            "'useLegacyRateConstants(0)' (MATLAB), 'Cantera::use_legacy_rate_constants"
            "(false)' (C++),\nor 'ct_use_legacy_rate_constants(0)' (clib).");

        processThirdBodies(m_ropf.data());
    }

    // copy result
    copy(m_ropf.begin(), m_ropf.end(), kfwd);
}

void GasKinetics::getJacobianSettings(AnyMap& settings) const
{
    settings["constant-pressure"] = m_jac_const_pressure;
    settings["mole-fractions"] = m_jac_mole_fractions;
    settings["skip-third-bodies"] = m_jac_skip_third_bodies;
    settings["skip-falloff"] = m_jac_skip_falloff;
    settings["rtol-delta-T"] = m_jac_rtol_deltaT;
}

void GasKinetics::setJacobianSettings(const AnyMap& settings)
{
    bool force = settings.empty();
    if (force || settings.hasKey("constant-pressure")) {
        m_jac_const_pressure = settings.getBool("constant-pressure", true);
    }
    if (force || settings.hasKey("mole-fractions")) {
        m_jac_mole_fractions = settings.getBool("mole-fractions", true);
    }
    if (force || settings.hasKey("skip-third-bodies")) {
        m_jac_skip_third_bodies = settings.getBool("skip-third-bodies", false);
    }
    if (force || settings.hasKey("skip-falloff")) {
        m_jac_skip_falloff = settings.getBool("skip-falloff", true);
    }
    if (!m_jac_skip_falloff) {
        m_jac_skip_falloff = true;
        throw NotImplementedError("GasKinetics::setJacobianSettings",
            "Derivative term related to reaction rate dependence on third bodies "
            "is not implemented.");
    }
    if (force || settings.hasKey("rtol-delta-T")) {
        m_jac_rtol_deltaT = settings.getDouble("rtol-delta-T", 1e-6);
    }
}

void GasKinetics::checkLegacyRates(const std::string& name)
{
    if (m_legacy.size()) {
        // Do not support legacy CTI/XML-based reaction rate evaluators
        throw CanteraError(name, "Not supported for legacy (CTI/XML) input format.");
    }
}

Eigen::VectorXd GasKinetics::fwdRateConstants_ddT()
{
    checkLegacyRates("GasKinetics::fwdRateConstants_ddT");
    updateROP();

    Eigen::VectorXd dFwdKc(nReactions());
    copy(m_rfn.begin(), m_rfn.end(), &dFwdKc[0]);
    Eigen::Map<Eigen::VectorXd> dFwdKcM(m_rbuf2.data(), nReactions());
    if (m_jac_const_pressure) {
        dFwdKcM = dFwdKc;
    }

    // direct temperature dependence
    for (auto& rates : m_bulk_rates) {
        rates->processRateConstants_ddT(
            dFwdKc.data(), m_rfn.data(), m_jac_rtol_deltaT);
    }

    // reaction rates that depend on third-body colliders
    if (m_jac_const_pressure) {
        for (auto& rates : m_bulk_rates) {
            rates->processRateConstants_ddM(
                dFwdKcM.data(), m_rfn.data(), m_jac_rtol_deltaT);
        }
        processConcentrations_ddT(dFwdKcM.data());
        dFwdKc += dFwdKcM;
    }
    return dFwdKc;
}

void GasKinetics::processEquilibriumConstants_ddT(double* drkcn)
{
    vector_fp& kc0 = m_rbuf0;
    vector_fp& kc1 = m_rbuf1;

    double T = thermo().temperature();
    double P = thermo().pressure();
    double dTinv = 1. / (m_jac_rtol_deltaT * T);
    thermo().setState_TP(T * (1. + m_jac_rtol_deltaT), P);
    getEquilibriumConstants(kc1.data());

    thermo().setState_TP(T, P);
    getEquilibriumConstants(kc0.data());

    for (size_t i = 0; i < nReactions(); ++i) {
        drkcn[i] *= (kc0[i] - kc1[i]) * dTinv;
        drkcn[i] /= kc0[i]; // divide once as this is a scaled derivative
    }

    for (size_t i = 0; i < m_irrev.size(); ++i) {
        drkcn[m_irrev[i]] = 0.0;
    }
}

void GasKinetics::processConcentrations_ddT(double* rop)
{
    double dctot_dT;
    if (thermo().type() == "IdealGas") {
        dctot_dT = -thermo().molarDensity() / thermo().temperature();
    } else {
        double T = thermo().temperature();
        double P = thermo().pressure();
        thermo().setState_TP(T * (1. + m_jac_rtol_deltaT), P);
        double ctot1 = thermo().molarDensity();
        thermo().setState_TP(T, P);
        double ctot0 = thermo().molarDensity();
        dctot_dT = (ctot1 - ctot0) / (T * m_jac_rtol_deltaT);
    }

    for (size_t i = 0; i < nReactions(); ++i ) {
        rop[i] *= dctot_dT;
    }
}

Eigen::VectorXd GasKinetics::fwdRatesOfProgress_ddT()
{
    checkLegacyRates("GasKinetics::fwdRatesOfProgress_ddT");
    updateROP();

    Eigen::VectorXd dFwdRop(nReactions());
    copy(m_ropf.begin(), m_ropf.end(), &dFwdRop[0]);
    for (auto& rates : m_bulk_rates) {
        rates->processRateConstants_ddT(
            dFwdRop.data(), m_rfn.data(), m_jac_rtol_deltaT);
    }

    if (m_jac_const_pressure) {
        Eigen::Map<Eigen::VectorXd> dFwdRopC(m_rbuf1.data(), nReactions());
        dFwdRopC.fill(0.);
        m_reactantStoich.scale(m_ropf.data(), dFwdRopC.data());

        // reaction rates that depend on third-body colliders
        Eigen::Map<Eigen::VectorXd> dFwdRopM(m_rbuf2.data(), nReactions());
        copy(m_ropf.begin(), m_ropf.end(), &dFwdRopM[0]);
        for (auto& rates : m_bulk_rates) {
            rates->processRateConstants_ddM(
                dFwdRopM.data(), m_rfn.data(), m_jac_atol_deltaT);
        }

        // reactions involving third body in law of mass action
        m_multi_concm.scaleOrder(m_ropf.data(), dFwdRopM.data());
        dFwdRopC += dFwdRopM;

        // add term to account for changes of concentrations
        processConcentrations_ddT(dFwdRopC.data());
        dFwdRop += dFwdRopC;
    }
    return dFwdRop;
}

Eigen::VectorXd GasKinetics::revRatesOfProgress_ddT()
{
    checkLegacyRates("GasKinetics::revRatesOfProgress_ddT");
    updateROP();

    // reverse rop times scaled rate constant derivative
    Eigen::VectorXd dRevRop(nReactions());
    copy(m_ropr.begin(), m_ropr.end(), &dRevRop[0]);
    for (auto& rates : m_bulk_rates) {
        rates->processRateConstants_ddT(
            dRevRop.data(), m_rfn.data(), m_jac_rtol_deltaT);
    }

    // reverse rop times scaled inverse equilibrium constant derivatives
    Eigen::Map<Eigen::VectorXd> dRevRop2(m_rbuf2.data(), nReactions());
    copy(m_ropr.begin(), m_ropr.end(), m_rbuf2.begin());
    processEquilibriumConstants_ddT(dRevRop2.data());

    dRevRop += dRevRop2;

    if (m_jac_const_pressure) {
        Eigen::Map<Eigen::VectorXd> dRevRopC(m_rbuf1.data(), nReactions());
        dRevRopC.fill(0.);
        m_revProductStoich.scale(m_ropr.data(), dRevRopC.data());

        // reaction rates that depend on third-body colliders
        Eigen::Map<Eigen::VectorXd> dRevRopM(m_rbuf2.data(), nReactions());
        copy(m_ropr.begin(), m_ropr.end(), &dRevRopM[0]);
        for (auto& rates : m_bulk_rates) {
            rates->processRateConstants_ddM(
                dRevRopM.data(), m_rfn.data(), m_jac_atol_deltaT);
        }

        // reactions involving third body in law of mass action
        m_multi_concm.scaleOrder(m_ropr.data(), dRevRopM.data());

        // add term to account for changes of concentrations
        processConcentrations_ddT(dRevRopC.data());
        dRevRop += dRevRopC;
    }

    return dRevRop;
}

Eigen::VectorXd GasKinetics::netRatesOfProgress_ddT()
{
    checkLegacyRates("GasKinetics::netRatesOfProgress_ddT");

    return fwdRatesOfProgress_ddT() - revRatesOfProgress_ddT();
}

void GasKinetics::scaleConcentrations(double *rates)
{
    double ctot = thermo().molarDensity();
    for (size_t i = 0; i < nReactions(); ++i) {
        rates[i] *= ctot;
    }
}

Eigen::SparseMatrix<double> GasKinetics::fwdRatesOfProgress_ddC()
{
    checkLegacyRates("GasKinetics::fwdRatesOfProgress_ddC");

    Eigen::SparseMatrix<double> jac;
    vector_fp& rop_rates = m_rbuf0;
    vector_fp& rop_stoich = m_rbuf1;
    vector_fp& rop_3b = m_rbuf2;

    // forward reaction rate coefficients
    processFwdRateCoefficients(rop_rates.data());
    if (m_jac_mole_fractions) {
        scaleConcentrations(rop_rates.data());
    }

    // derivatives handled by StoichManagerN
    copy(rop_rates.begin(), rop_rates.end(), rop_stoich.begin());
    processThirdBodies(rop_stoich.data());
    jac = m_reactantStoich.jacobian(m_act_conc.data(), rop_stoich.data());

    // derivatives handled by ThirdBodyCalc
    if (!m_jac_skip_third_bodies && !m_concm.empty()) {
        copy(rop_rates.begin(), rop_rates.end(), rop_3b.begin());
        m_reactantStoich.multiply(m_act_conc.data(), rop_3b.data());
        jac += m_multi_concm.jacobian(rop_3b.data());
    }

    return jac;
}

Eigen::SparseMatrix<double> GasKinetics::revRatesOfProgress_ddC()
{
    checkLegacyRates("GasKinetics::revRatesOfProgress_ddC");

    Eigen::SparseMatrix<double> jac;
    vector_fp& rop_rates = m_rbuf0;
    vector_fp& rop_stoich = m_rbuf1;
    vector_fp& rop_3b = m_rbuf2;

    // reverse reaction rate coefficients
    processFwdRateCoefficients(rop_rates.data());
    processEquilibriumConstants(rop_rates.data());
    if (m_jac_mole_fractions) {
        scaleConcentrations(rop_rates.data());
    }

    // derivatives handled by StoichManagerN
    copy(rop_rates.begin(), rop_rates.end(), rop_stoich.begin());
    processThirdBodies(rop_stoich.data());
    jac = m_revProductStoich.jacobian(m_act_conc.data(), rop_stoich.data());

    // derivatives handled by ThirdBodyCalc
    if (!m_jac_skip_third_bodies && !m_concm.empty()) {
        copy(rop_rates.begin(), rop_rates.end(), rop_3b.begin());
        m_revProductStoich.multiply(m_act_conc.data(), rop_3b.data());
        jac += m_multi_concm.jacobian(rop_3b.data());
    }

    return jac;
}

Eigen::SparseMatrix<double> GasKinetics::netRatesOfProgress_ddC()
{
    checkLegacyRates("GasKinetics::netRatesOfProgress_ddC");

    Eigen::SparseMatrix<double> jac;
    vector_fp& rop_rates = m_rbuf0;
    vector_fp& rop_stoich = m_rbuf1;
    vector_fp& rop_3b = m_rbuf2;

    // forward reaction rate coefficients
    processFwdRateCoefficients(rop_rates.data());
    if (m_jac_mole_fractions) {
        scaleConcentrations(rop_rates.data());
    }
    copy(rop_rates.begin(), rop_rates.end(), rop_stoich.begin());

    // forward derivatives handled by StoichManagerN
    processThirdBodies(rop_stoich.data());
    jac = m_reactantStoich.jacobian(m_act_conc.data(), rop_stoich.data());

    // forward derivatives handled by ThirdBodyCalc
    if (!m_jac_skip_third_bodies && !m_concm.empty()) {
        copy(rop_rates.begin(), rop_rates.end(), rop_3b.begin());
        m_reactantStoich.multiply(m_act_conc.data(), rop_3b.data());
        jac += m_multi_concm.jacobian(rop_3b.data());
    }

    // reverse reaction rate coefficients
    processEquilibriumConstants(rop_rates.data());
    copy(rop_rates.begin(), rop_rates.end(), rop_stoich.begin());

    // reverse derivatives handled by StoichManagerN
    processThirdBodies(rop_stoich.data());
    jac -= m_revProductStoich.jacobian(m_act_conc.data(), rop_stoich.data());

    // reverse derivatives handled by ThirdBodyCalc
    if (!m_jac_skip_third_bodies && !m_concm.empty()) {
        copy(rop_rates.begin(), rop_rates.end(), rop_3b.begin());
        m_revProductStoich.multiply(m_act_conc.data(), rop_3b.data());
        jac -= m_multi_concm.jacobian(rop_3b.data());
    }

    return jac;
}

bool GasKinetics::addReaction(shared_ptr<Reaction> r, bool resize)
{
    // operations common to all reaction types
    bool added = BulkKinetics::addReaction(r, resize);
    if (!added) {
        return false;
    } else if (!(r->usesLegacy())) {
        // Rate object already added in BulkKinetics::addReaction
        return true;
    }

    if (r->type() == "elementary-legacy") {
        addElementaryReaction(dynamic_cast<ElementaryReaction2&>(*r));
    } else if (r->type() == "three-body-legacy") {
        addThreeBodyReaction(dynamic_cast<ThreeBodyReaction2&>(*r));
    } else if (r->type() == "falloff-legacy") {
        addFalloffReaction(dynamic_cast<FalloffReaction2&>(*r));
    } else if (r->type() == "chemically-activated-legacy") {
        addFalloffReaction(dynamic_cast<FalloffReaction2&>(*r));
    } else if (r->type() == "pressure-dependent-Arrhenius-legacy") {
        addPlogReaction(dynamic_cast<PlogReaction2&>(*r));
    } else if (r->type() == "Chebyshev-legacy") {
        addChebyshevReaction(dynamic_cast<ChebyshevReaction2&>(*r));
    } else {
        throw CanteraError("GasKinetics::addReaction",
            "Unknown reaction type specified: '{}'", r->type());
    }
    m_legacy.push_back(nReactions() - 1);
    return true;
}

void GasKinetics::addFalloffReaction(FalloffReaction& r)
{
    // install high and low rate coeff calculators and extend the high and low
    // rate coeff value vectors
    size_t nfall = m_falloff_high_rates.nReactions();
    m_falloff_high_rates.install(nfall, r.high_rate);
    m_rfn_high.push_back(0.0);
    m_falloff_low_rates.install(nfall, r.low_rate);
    m_rfn_low.push_back(0.0);

    // add this reaction number to the list of falloff reactions
    m_fallindx.push_back(nReactions()-1);
    m_rfallindx[nReactions()-1] = nfall;

    // install the enhanced third-body concentration calculator
    map<size_t, double> efficiencies;
    for (const auto& eff : r.third_body.efficiencies) {
        size_t k = kineticsSpeciesIndex(eff.first);
        if (k != npos) {
            efficiencies[k] = eff.second;
        }
    }
    m_falloff_concm.install(nfall, efficiencies,
                            r.third_body.default_efficiency,
                            nReactions() - 1);
    concm_falloff_values.resize(m_falloff_concm.workSize());

    // install the falloff function calculator for this reaction
    m_falloffn.install(nfall, r.type(), r.falloff);
    falloff_work.resize(m_falloffn.workSize());
}

void GasKinetics::addThreeBodyReaction(ThreeBodyReaction2& r)
{
    m_rates.install(nReactions()-1, r.rate);
    map<size_t, double> efficiencies;
    for (const auto& eff : r.third_body.efficiencies) {
        size_t k = kineticsSpeciesIndex(eff.first);
        if (k != npos) {
            efficiencies[k] = eff.second;
        }
    }
    m_3b_concm.install(nReactions()-1, efficiencies,
                       r.third_body.default_efficiency);
    concm_3b_values.resize(m_3b_concm.workSize());
}

void GasKinetics::addPlogReaction(PlogReaction2& r)
{
    m_plog_rates.install(nReactions()-1, r.rate);
}

void GasKinetics::addChebyshevReaction(ChebyshevReaction2& r)
{
    m_cheb_rates.install(nReactions()-1, r.rate);
}

void GasKinetics::modifyReaction(size_t i, shared_ptr<Reaction> rNew)
{
    // operations common to all bulk reaction types
    BulkKinetics::modifyReaction(i, rNew);

    // invalidate all cached data
    invalidateCache();

    if (!(rNew->usesLegacy())) {
        // Rate object already modified in BulkKinetics::modifyReaction
        return;
    }

    if (rNew->type() == "elementary-legacy") {
        modifyElementaryReaction(i, dynamic_cast<ElementaryReaction2&>(*rNew));
    } else if (rNew->type() == "three-body-legacy") {
        modifyThreeBodyReaction(i, dynamic_cast<ThreeBodyReaction2&>(*rNew));
    } else if (rNew->type() == "falloff-legacy") {
        modifyFalloffReaction(i, dynamic_cast<FalloffReaction2&>(*rNew));
    } else if (rNew->type() == "chemically-activated-legacy") {
        modifyFalloffReaction(i, dynamic_cast<FalloffReaction2&>(*rNew));
    } else if (rNew->type() == "pressure-dependent-Arrhenius-legacy") {
        modifyPlogReaction(i, dynamic_cast<PlogReaction2&>(*rNew));
    } else if (rNew->type() == "Chebyshev-legacy") {
        modifyChebyshevReaction(i, dynamic_cast<ChebyshevReaction2&>(*rNew));
    } else {
        throw CanteraError("GasKinetics::modifyReaction",
            "Unknown reaction type specified: '{}'", rNew->type());
    }
}

void GasKinetics::modifyThreeBodyReaction(size_t i, ThreeBodyReaction2& r)
{
    m_rates.replace(i, r.rate);
}

void GasKinetics::modifyFalloffReaction(size_t i, FalloffReaction& r)
{
    size_t iFall = m_rfallindx[i];
    m_falloff_high_rates.replace(iFall, r.high_rate);
    m_falloff_low_rates.replace(iFall, r.low_rate);
    m_falloffn.replace(iFall, r.falloff);
}

void GasKinetics::modifyPlogReaction(size_t i, PlogReaction2& r)
{
    m_plog_rates.replace(i, r.rate);
}

void GasKinetics::modifyChebyshevReaction(size_t i, ChebyshevReaction2& r)
{
    m_cheb_rates.replace(i, r.rate);
}

void GasKinetics::invalidateCache()
{
    BulkKinetics::invalidateCache();
    m_pres += 0.13579;
}

}
