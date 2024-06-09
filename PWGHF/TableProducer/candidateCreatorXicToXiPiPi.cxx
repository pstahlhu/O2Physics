// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file candidateCreatorXicToXiPiPi.cxx
/// \brief Reconstruction of Ξc± → (Ξ∓ → (Λ → p π∓) π∓) π± π± candidates
///
/// \author Phil Lennart Stahlhut <phil.lennart.stahlhut@cern.ch>, Heidelberg University
/// \author Jinjoo Seo <jseo@cern.ch>, Heidelberg University

#ifndef HomogeneousField
#define HomogeneousField
#endif

#include <KFParticleBase.h>
#include <KFParticle.h>
#include <KFPTrack.h>
#include <KFPVertex.h>
#include <KFVertex.h>

#include <TPDGCode.h>

#include "CommonConstants/PhysicsConstants.h"
#include "DCAFitter/DCAFitterN.h"
#include "Framework/AnalysisTask.h"
#include "Framework/runDataProcessing.h"
#include "ReconstructionDataFormats/DCA.h"
#include "ReconstructionDataFormats/V0.h" // for creating XicPlus track with DCA fitter

#include "Common/DataModel/CollisionAssociationTables.h"
#include "Common/Core/trackUtilities.h"
#include "Tools/KFparticle/KFUtilities.h"

#include "PWGLF/DataModel/LFStrangenessTables.h"

#include "PWGHF/DataModel/CandidateReconstructionTables.h"
#include "PWGHF/Utils/utilsBfieldCCDB.h"

using namespace o2;
using namespace o2::analysis;
using namespace o2::aod::hf_cand_xictoxipipi;
using namespace o2::constants::physics;
using namespace o2::framework;
using namespace o2::framework::expressions;

/// Reconstruction of heavy-flavour 3-prong decay candidates
struct HfCandidateCreatorXic {
  Produces<aod::HfCandXicBase> rowCandidateBase;
  Produces<aod::HfCandXicKF> rowCandidateKF;

  Configurable<bool> fillHistograms{"fillHistograms", true, "do validation plots"};
  // magnetic field setting from CCDB
  Configurable<bool> isRun2{"isRun2", false, "enable Run 2 or Run 3 GRP objects for magnetic field"};
  Configurable<std::string> ccdbUrl{"ccdbUrl", "http://alice-ccdb.cern.ch", "url of the ccdb repository"};
  Configurable<std::string> ccdbPathLut{"ccdbPathLut", "GLO/Param/MatLUT", "Path for LUT parametrization"};
  Configurable<std::string> ccdbPathGrp{"ccdbPathGrp", "GLO/GRP/GRP", "Path of the grp file (Run 2)"};
  Configurable<std::string> ccdbPathGrpMag{"ccdbPathGrpMag", "GLO/Config/GRPMagField", "CCDB path of the GRPMagField object (Run 3)"};
  // cascade preselections
  Configurable<bool> doCascadePreselection{"doCascadePreselection", true, "Use invariant mass and dcaXY cuts to preselect cascade candidates"};
  Configurable<double> massToleranceCascade{"massToleranceCascade", 0.01, "Invariant mass tolerance for cascade"};
  Configurable<float> dcaXYToPVCascadeMax{"dcaXYToPVCascadeMax", 3, "Max cascade DCA to PV in xy plane"};
  // DCA fitter
  Configurable<bool> propagateToPCA{"propagateToPCA", true, "create tracks version propagated to PCA"};
  Configurable<double> maxR{"maxR", 200., "reject PCA's above this radius"};
  Configurable<double> maxDZIni{"maxDZIni", 4., "reject (if>0) PCA candidate if tracks DZ exceeds threshold"};
  Configurable<double> minParamChange{"minParamChange", 1.e-3, "stop iterations if largest change of any X is smaller than this"};
  Configurable<double> minRelChi2Change{"minRelChi2Change", 0.9, "stop iterations is chi2/chi2old > this"};
  Configurable<bool> useAbsDCA{"useAbsDCA", false, "Minimise abs. distance rather than chi2"};
  Configurable<bool> useWeightedFinalPCA{"useWeightedFinalPCA", false, "Recalculate vertex position using track covariances, effective only if useAbsDCA is true"};
  //  KFParticle
  Configurable<bool> constrainXicPlusToPv{"constrainXicPlusToPv", false, "Constrain XicPlus to PV"};
  Configurable<int> kfConstructMethod{"kfConstructMethod", 0, "Construct method of XicPlus: 0 no mass constraint, 2 mass constraint"};
  Configurable<bool> rejDiffCollTrack{"rejDiffCollTrack", true, "Reject tracks coming from different collisions (effective only for KFParticle w/o derived data)"};

  Service<o2::ccdb::BasicCCDBManager> ccdb;
  o2::base::MatLayerCylSet* lut;
  o2::base::Propagator::MatCorrType matCorr = o2::base::Propagator::MatCorrType::USEMatCorrLUT;

  double massXiMinusFromPdg = MassXiMinus;
  double massPionFromPdg = MassPiPlus;

  int runNumber{0};
  float massXiPiPi{0.};
  float massXiPi0{0.};
  float massXiPi1{0.};
  double bz{0.};

  using CascadesLinked = soa::Join<aod::Cascades, aod::CascDataLink>;
  using CascFull = soa::Join<aod::CascDatas, aod::CascCovs>;
  using KFCascadesLinked = soa::Join<aod::Cascades, aod::KFCascDataLink>;
  using KFCascFull = soa::Join<aod::KFCascDatas, aod::KFCascCovs>;
  using SelectedCollisions = soa::Filtered<soa::Join<aod::Collisions, aod::HfSelCollision>>;
  using SelectedHfTrackAssoc = soa::Filtered<soa::Join<aod::TrackAssoc, aod::HfSelTrack>>;

  Filter filterSelectCollisions = (aod::hf_sel_collision::whyRejectColl == static_cast<uint16_t>(0));
  Filter filterSelectTrackIds = ((aod::hf_sel_track::isSelProng & static_cast<uint32_t>(BIT(4))) != 0u); // corresponds to CandidateType::CandCascadeBachelor in trackIndexSkimCreator.cxx

  Preslice<SelectedHfTrackAssoc> trackIndicesPerCollision = aod::track_association::collisionId;
  Preslice<KFCascadesLinked> linkedCascadesPerCollision = aod::cascdata::collisionId;

  OutputObj<TH1F> hMass3{TH1F("hMass3", "3-prong candidates;inv. mass (#Xi #pi #pi) (GeV/#it{c}^{2});entries", 500, 2.3, 2.7)};
  OutputObj<TH1F> hCovPVXX{TH1F("hCovPVXX", "3-prong candidates;XX element of cov. matrix of prim. vtx. position (cm^{2});entries", 100, 0., 1.e-4)};
  OutputObj<TH1F> hCovSVXX{TH1F("hCovSVXX", "3-prong candidates;XX element of cov. matrix of sec. vtx. position (cm^{2});entries", 100, 0., 0.2)};
  OutputObj<TH1F> hCovPVYY{TH1F("hCovPVYY", "3-prong candidates;YY element of cov. matrix of prim. vtx. position (cm^{2});entries", 100, 0., 1.e-4)};
  OutputObj<TH1F> hCovSVYY{TH1F("hCovSVYY", "3-prong candidates;YY element of cov. matrix of sec. vtx. position (cm^{2});entries", 100, 0., 0.2)};
  OutputObj<TH1F> hCovPVXZ{TH1F("hCovPVXZ", "3-prong candidates;XZ element of cov. matrix of prim. vtx. position (cm^{2});entries", 100, -1.e-4, 1.e-4)};
  OutputObj<TH1F> hCovSVXZ{TH1F("hCovSVXZ", "3-prong candidates;XZ element of cov. matrix of sec. vtx. position (cm^{2});entries", 100, -1.e-4, 0.2)};
  OutputObj<TH1F> hCovPVZZ{TH1F("hCovPVZZ", "3-prong candidates;ZZ element of cov. matrix of prim. vtx. position (cm^{2});entries", 100, 0., 1.e-4)};
  OutputObj<TH1F> hCovSVZZ{TH1F("hCovSVZZ", "3-prong candidates;ZZ element of cov. matrix of sec. vtx. position (cm^{2});entries", 100, 0., 0.2)};
  OutputObj<TH2F> hDcaXYProngs{TH2F("hDcaXYProngs", "DCAxy of 3-prong candidates;#it{p}_{T} (GeV/#it{c};#it{d}_{xy}) (#mum);entries", 100, 0., 20., 200, -500., 500.)};
  OutputObj<TH2F> hDcaZProngs{TH2F("hDcaZProngs", "DCAz of 3-prong candidates;#it{p}_{T} (GeV/#it{c};#it{d}_{z}) (#mum);entries", 100, 0., 20., 200, -500., 500.)};
  OutputObj<TH1F> hVertexerType{TH1F("hVertexerType", "Use KF or DCAFitterN;Vertexer type;entries", 2, -0.5, 1.5)}; // See o2::aod::hf_cand::VertexerType

  void init(InitContext const&)
  {
    ccdb->setURL(ccdbUrl);
    ccdb->setCaching(true);
    ccdb->setLocalObjectValidityChecking();
    lut = o2::base::MatLayerCylSet::rectifyPtrFromFile(ccdb->get<o2::base::MatLayerCylSet>(ccdbPathLut));
    runNumber = 0;

    std::array<bool, 2> doprocessKF{doprocessXicplusWithKFParticleFromDerivedData, doprocessXicplusWithKFParticle};
    if ((doprocessXicplusWithDcaFitter + std::accumulate(doprocessKF.begin(), doprocessKF.end(), 0)) != 1) {
      LOGP(fatal, "Only one process function can be enabled at a time.");
    }
    if ((doprocessXicplusWithDcaFitter == 1) && fillHistograms) {
      hVertexerType->Fill(aod::hf_cand::VertexerType::DCAFitter);
    }
    if ((std::accumulate(doprocessKF.begin(), doprocessKF.end(), 0) == 1) && fillHistograms) {
      hVertexerType->Fill(aod::hf_cand::VertexerType::KfParticle);
    }
  }

  void processXicplusWithDcaFitter(aod::Collisions const&,
                                   aod::HfCascLf3Prongs const& rowsTrackIndexXicPlus,
                                   CascadesLinked const&,
                                   CascFull const&,
                                   aod::TracksWCovDca const&,
                                   aod::BCsWithTimestamps const&)
  {
    // initialize 3-prong vertex fitter
    o2::vertexing::DCAFitterN<3> df;
    df.setPropagateToPCA(propagateToPCA);
    df.setMaxR(maxR);
    df.setMaxDZIni(maxDZIni);
    df.setMinParamChange(minParamChange);
    df.setMinRelChi2Change(minRelChi2Change);
    df.setUseAbsDCA(useAbsDCA);
    df.setWeightedFinalPCA(useWeightedFinalPCA);

    // loop over triplets of track indices
    for (const auto& rowTrackIndexXicPlus : rowsTrackIndexXicPlus) {
      auto cascAodElement = rowTrackIndexXicPlus.cascade_as<CascadesLinked>();
      if (!cascAodElement.has_cascData()) {
        continue;
      }
      auto casc = cascAodElement.cascData_as<CascFull>();
      auto trackCharmBachelor0 = rowTrackIndexXicPlus.prong0_as<aod::TracksWCovDca>();
      auto trackCharmBachelor1 = rowTrackIndexXicPlus.prong1_as<aod::TracksWCovDca>();
      auto collision = rowTrackIndexXicPlus.collision();

      // preselect cascade candidates
      if (doCascadePreselection) {
        if (std::abs(casc.dcaXYCascToPV()) > dcaXYToPVCascadeMax) {
          continue;
        }
        if (std::abs(casc.mXi() - massXiMinusFromPdg) > massToleranceCascade) {
          continue;
        }
      }

      //----------------------Set the magnetic field from ccdb---------------------------------------
      /// The static instance of the propagator was already modified in the HFTrackIndexSkimCreator,
      /// but this is not true when running on Run2 data/MC already converted into AO2Ds.
      auto bc = collision.bc_as<aod::BCsWithTimestamps>();
      if (runNumber != bc.runNumber()) {
        LOG(info) << ">>>>>>>>>>>> Current run number: " << runNumber;
        initCCDB(bc, runNumber, ccdb, isRun2 ? ccdbPathGrp : ccdbPathGrpMag, lut, isRun2);
        bz = o2::base::Propagator::Instance()->getNominalBz();
        LOG(info) << ">>>>>>>>>>>> Magnetic field: " << bz;
      }
      df.setBz(bz);

      //----------------accessing particles in the decay chain-------------
      auto trackPionFromXi = casc.bachelor_as<aod::TracksWCovDca>(); // pion <- xi track from TracksWCovDca table

      //--------------------------info of V0 and cascades track from LF-tables---------------------------
      std::array<float, 3> vertexV0 = {casc.xlambda(), casc.ylambda(), casc.zlambda()};
      std::array<float, 3> pVecV0 = {casc.pxlambda(), casc.pylambda(), casc.pzlambda()};
      std::array<float, 3> vertexCasc = {casc.x(), casc.y(), casc.z()};
      std::array<float, 3> pVecCasc = {casc.px(), casc.py(), casc.pz()};
      std::array<float, 21> covCasc = {0.};

      //----------------create cascade track------------------------------------------------------------
      constexpr int MomInd[6] = {9, 13, 14, 18, 19, 20}; // cov matrix elements for momentum component
      for (int i = 0; i < 6; i++) {
        covCasc[MomInd[i]] = casc.momentumCovMat()[i];
        covCasc[i] = casc.positionCovMat()[i];
      }
      // create cascade track
      o2::track::TrackParCov trackCasc;
      if (trackPionFromXi.sign() > 0) {
        trackCasc = o2::track::TrackParCov(vertexCasc, pVecCasc, covCasc, 1, true);
      } else if (trackPionFromXi.sign() < 0) {
        trackCasc = o2::track::TrackParCov(vertexCasc, pVecCasc, covCasc, -1, true);
      } else {
        continue;
      }
      trackCasc.setAbsCharge(1);
      trackCasc.setPID(o2::track::PID::XiMinus);

      //----------------------------fit SV and create XicPlus track------------------
      auto trackParCovCharmBachelor0 = getTrackParCov(trackCharmBachelor0);
      auto trackParCovCharmBachelor1 = getTrackParCov(trackCharmBachelor1);

      // reconstruct the 3-prong secondary vertex
      if (df.process(trackCasc, trackParCovCharmBachelor0, trackParCovCharmBachelor1) == 0) {
        continue;
      }

      //----------------------------calculate physical properties-----------------------
      // set hfFlag
      int hfFlag = BIT(aod::hf_cand_xictoxipipi::DecayType::XicToXiPiPi);

      // Charge of charm baryon
      int signXic = casc.sign() < 0 ? +1 : -1;

      // get SV properties
      const auto& secondaryVertex = df.getPCACandidate();
      auto chi2SV = df.getChi2AtPCACandidate();
      auto covMatrixSV = df.calcPCACovMatrixFlat();

      // get track momenta
      trackParCovCharmBachelor0 = df.getTrack(1);
      trackParCovCharmBachelor1 = df.getTrack(2);
      trackCasc = df.getTrack(2);
      std::array<float, 3> pVecXi;
      std::array<float, 3> pVecPi0;
      std::array<float, 3> pVecPi1;
      trackCasc.getPxPyPzGlo(pVecXi);
      trackParCovCharmBachelor0.getPxPyPzGlo(pVecPi0);
      trackParCovCharmBachelor1.getPxPyPzGlo(pVecPi1);

      // get invariant mass of Xic candidate
      auto arrayMomenta = std::array{pVecXi, pVecPi0, pVecPi1};
      massXiPiPi = RecoDecay::m(std::move(arrayMomenta), std::array{massXiMinusFromPdg, massPionFromPdg, massPionFromPdg});

      /* get parent track necessary?
      std::array<float, 3> pVec2Pi = RecoDecay::pVec(pVecPi0, pVecPi1);
      std::array<float, 3> pVecXicPlus = RecoDecay::pVec(pVecXi, pVecPi0, pVecPi1);
      auto trackParCov2Pi = o2::dataformats::V0(df.getPCACandidatePos(), pVec2Pi, df.calcPCACovMatrixFlat(), trackParCovCharmBachelor0, trackParCovCharmBachelor1);
      auto trackParCovXicPlus = o2::dataformats::V0(df.getPCACandidatePos(), pVecXicPlus, df.calcPCACovMatrixFlat(), trackParCov2Pi, trackCasc);
      trackParCovXicPlus.getPxPyPzGlo(pVecXicPlus);*/

      // get track impact parameters
      // This modifies track momenta!
      auto primaryVertex = getPrimaryVertex(collision);
      auto covMatrixPV = primaryVertex.getCov();
      // calculate impact parameter
      o2::dataformats::DCA impactParameterCasc;
      o2::dataformats::DCA impactParameter0;
      o2::dataformats::DCA impactParameter1;
      trackCasc.propagateToDCA(primaryVertex, bz, &impactParameterCasc);
      trackParCovCharmBachelor0.propagateToDCA(primaryVertex, bz, &impactParameter0);
      trackParCovCharmBachelor1.propagateToDCA(primaryVertex, bz, &impactParameter1);

      // calculate cosine of pointing angle
      std::array<float, 3> pvCoord = {collision.posX(), collision.posY(), collision.posZ()};
      double cpaLambda = casc.v0cosPA(collision.posX(), collision.posY(), collision.posZ());
      double cpaXYLambda = RecoDecay::cpaXY(pvCoord, vertexV0, pVecV0);
      double cpaXi = casc.casccosPA(collision.posX(), collision.posY(), collision.posZ());
      double cpaXYXi = RecoDecay::cpaXY(pvCoord, vertexCasc, pVecCasc);

      // get invariant mass of Xi-pi pairs
      auto arrayMomentaXiPi0 = std::array{pVecXi, pVecPi0};
      massXiPi0 = RecoDecay::m(std::move(arrayMomentaXiPi0), std::array{massXiMinusFromPdg, massPionFromPdg});
      auto arrayMomentaXiPi1 = std::array{pVecXi, pVecPi1};
      massXiPi1 = RecoDecay::m(std::move(arrayMomentaXiPi1), std::array{massXiMinusFromPdg, massPionFromPdg});

      // get uncertainty of the decay length
      double phi, theta;
      getPointDirection(std::array{primaryVertex.getX(), primaryVertex.getY(), primaryVertex.getZ()}, secondaryVertex, phi, theta);
      auto errorDecayLength = std::sqrt(getRotatedCovMatrixXX(covMatrixPV, phi, theta) + getRotatedCovMatrixXX(covMatrixSV, phi, theta));
      auto errorDecayLengthXY = std::sqrt(getRotatedCovMatrixXX(covMatrixPV, phi, 0.) + getRotatedCovMatrixXX(covMatrixSV, phi, 0.));

      //--------------------------------------------fill histograms----------------------------------------------------------------
      if (fillHistograms) {
        // invariant mass
        hMass3->Fill(massXiPiPi);
        // covariance matrix elements of PV
        hCovPVXX->Fill(covMatrixPV[0]);
        hCovPVYY->Fill(covMatrixPV[2]);
        hCovPVXZ->Fill(covMatrixPV[3]);
        hCovPVZZ->Fill(covMatrixPV[5]);
        // covariance matrix elements of SV
        hCovSVXX->Fill(covMatrixSV[0]);
        hCovSVYY->Fill(covMatrixSV[2]);
        hCovSVXZ->Fill(covMatrixSV[3]);
        hCovSVZZ->Fill(covMatrixSV[5]);
        // DCAs of prongs
        hDcaXYProngs->Fill(trackCharmBachelor0.pt(), impactParameter0.getY());
        hDcaXYProngs->Fill(trackCharmBachelor1.pt(), impactParameter1.getY());
        hDcaXYProngs->Fill(trackCasc.getPt(), impactParameterCasc.getY());
        hDcaZProngs->Fill(trackCharmBachelor0.pt(), impactParameter0.getZ());
        hDcaZProngs->Fill(trackCharmBachelor1.pt(), impactParameter1.getZ());
        hDcaZProngs->Fill(trackCasc.getPt(), impactParameterCasc.getZ());
      }

      //---------------------------------fill candidate table rows-------------------------------------------------------------------------------------------
      rowCandidateBase(collision.globalIndex(),
                       primaryVertex.getX(), primaryVertex.getY(), primaryVertex.getZ(),
                       covMatrixPV[0], covMatrixPV[2], covMatrixPV[5],
                       /*3-prong specific columns*/
                       rowTrackIndexXicPlus.cascadeId(), rowTrackIndexXicPlus.prong0Id(), rowTrackIndexXicPlus.prong1Id(),
                       casc.bachelorId(), casc.posTrackId(), casc.negTrackId(),
                       secondaryVertex[0], secondaryVertex[1], secondaryVertex[2],
                       covMatrixSV[0], covMatrixSV[2], covMatrixSV[5],
                       errorDecayLength, errorDecayLengthXY,
                       chi2SV, massXiPiPi, signXic,
                       pVecXi[0], pVecXi[1], pVecXi[2],
                       pVecPi0[0], pVecPi0[1], pVecPi0[2],
                       pVecPi1[0], pVecPi1[1], pVecPi1[2],
                       impactParameterCasc.getY(), impactParameter0.getY(), impactParameter1.getY(),
                       std::sqrt(impactParameterCasc.getSigmaY2()), std::sqrt(impactParameter0.getSigmaY2()), std::sqrt(impactParameter1.getSigmaY2()),
                       hfFlag,
                       /*cascade specific columns*/
                       vertexCasc[0], vertexCasc[1], vertexCasc[2],
                       vertexV0[0], vertexV0[1], vertexV0[2],
                       cpaXi, cpaXYXi, cpaLambda, cpaXYLambda,
                       massXiPi0, massXiPi1);
    } // loop over track triplets
  }
  PROCESS_SWITCH(HfCandidateCreatorXic, processXicplusWithDcaFitter, "Run candidate creator with DCAFitter.", true);

  void processXicplusWithKFParticleFromDerivedData(aod::Collisions const&,
                                                   aod::HfCascLf3Prongs const& rowsTrackIndexXicPlus,
                                                   KFCascadesLinked const&,
                                                   KFCascFull const&,
                                                   aod::TracksWCovExtra const&,
                                                   aod::BCsWithTimestamps const&)
  {
    // loop over triplets of track indices
    for (const auto& rowTrackIndexXicPlus : rowsTrackIndexXicPlus) {
      auto cascAodElement = rowTrackIndexXicPlus.cascade_as<aod::KFCascadesLinked>();
      if (!cascAodElement.has_kfCascData()) {
        continue;
      }
      auto casc = cascAodElement.kfCascData_as<KFCascFull>();
      auto trackCharmBachelor0 = rowTrackIndexXicPlus.prong0_as<aod::TracksWCovExtra>();
      auto trackCharmBachelor1 = rowTrackIndexXicPlus.prong1_as<aod::TracksWCovExtra>();
      auto collision = rowTrackIndexXicPlus.collision();

      //-------------------preselect cascade candidates--------------------------------------
      if (doCascadePreselection) {
        if (std::abs(casc.dcaXYCascToPV()) > dcaXYToPVCascadeMax) {
          continue;
        }
        if (std::abs(casc.mXi() - massXiMinusFromPdg) > massToleranceCascade) {
          continue;
        }
      }

      //----------------------Set the magnetic field from ccdb-----------------------------
      /// The static instance of the propagator was already modified in the HFTrackIndexSkimCreator,
      /// but this is not true when running on Run2 data/MC already converted into AO2Ds.
      auto bc = collision.bc_as<aod::BCsWithTimestamps>();
      if (runNumber != bc.runNumber()) {
        LOG(info) << ">>>>>>>>>>>> Current run number: " << runNumber;
        initCCDB(bc, runNumber, ccdb, isRun2 ? ccdbPathGrp : ccdbPathGrpMag, lut, isRun2);
        bz = o2::base::Propagator::Instance()->getNominalBz();
        LOG(info) << ">>>>>>>>>>>> Magnetic field: " << bz;
      }
      KFParticle::SetField(bz);

      //----------------------info of V0 and cascade tracks from LF-table------------------
      std::array<float, 3> vertexV0 = {casc.xlambda(), casc.ylambda(), casc.zlambda()};
      std::array<float, 3> pVecV0 = {casc.pxlambda(), casc.pylambda(), casc.pzlambda()};
      std::array<float, 3> vertexCasc = {casc.x(), casc.y(), casc.z()};
      std::array<float, 3> pVecCasc = {casc.px(), casc.py(), casc.pz()};

      //----------------------Create XicPlus as KFParticle object-------------------------------------------
      // initialize primary vertex
      KFPVertex kfpVertex = createKFPVertexFromCollision(collision);
      float covMatrixPV[6];
      kfpVertex.GetCovarianceMatrix(covMatrixPV);
      KFParticle KFPV(kfpVertex); // for calculation of DCAs to PV

      // convert pion tracks into KFParticle object
      KFPTrack kfpTrackCharmBachelor0 = createKFPTrackFromTrack(trackCharmBachelor0);
      KFPTrack kfpTrackCharmBachelor1 = createKFPTrackFromTrack(trackCharmBachelor1);
      KFParticle kfCharmBachelor0(kfpTrackCharmBachelor0, kPiPlus);
      KFParticle kfCharmBachelor1(kfpTrackCharmBachelor1, kPiPlus);

      // create Xi as KFParticle object
      // read {X,Y,Z,Px,Py,Pz} and corresponding covariance matrix from KF cascade Tables
      std::array<float, 6> xyzpxpypz = {casc.x(), casc.y(), casc.z(), casc.px(), casc.py(), casc.pz()};
      float parPosMom[6];
      for (int i{0}; i < 6; ++i) {
        parPosMom[i] = xyzpxpypz[i];
      }
      // create KFParticle
      KFParticle kfXi;
      kfXi.Create(parPosMom, casc.kfTrackCovMat(), casc.sign(), casc.mXi());

      // create XicPlus as KFParticle object
      KFParticle kfXicPlus;
      const KFParticle* kfDaughtersXicPlus[3] = {&kfCharmBachelor0, &kfCharmBachelor1, &kfXi};
      kfXicPlus.SetConstructMethod(kfConstructMethod);
      try {
        kfXicPlus.Construct(kfDaughtersXicPlus, 3);
      } catch (std::runtime_error& e) {
        LOG(debug) << "Failed to construct XicPlus : " << e.what();
        continue;
      }

      // topological constraint
      if (constrainXicPlusToPv) {
        kfXicPlus.SetProductionVertex(KFPV);
      }
      auto covMatrixXicPlus = kfXicPlus.CovarianceMatrix();

      // transport daughter particles to XicPlus decay vertex
      kfCharmBachelor0.TransportToParticle(kfXicPlus);
      kfCharmBachelor1.TransportToParticle(kfXicPlus);
      kfXi.TransportToParticle(kfXicPlus);

      //---------------------calculate physical parameters of XicPlus candidate----------------------
      // sign of charm baryon
      int signXic = casc.sign() < 0 ? +1 : -1;

      // set hfFlag
      int hfFlag = BIT(aod::hf_cand_xictoxipipi::DecayType::XicToXiPiPi);

      // get impact parameters of XicPlus daughters
      float impactParameterPi0XY = 0., errImpactParameterPi0XY = 0.;
      float impactParameterPi1XY = 0., errImpactParameterPi1XY = 0.;
      float impactParameterXiXY = 0., errImpactParameterXiXY = 0.;
      kfCharmBachelor0.GetDistanceFromVertexXY(KFPV, impactParameterPi0XY, errImpactParameterPi0XY);
      kfCharmBachelor1.GetDistanceFromVertexXY(KFPV, impactParameterPi1XY, errImpactParameterPi1XY);
      kfXi.GetDistanceFromVertexXY(KFPV, impactParameterXiXY, errImpactParameterXiXY);

      // calculate cosine of pointing angle
      std::array<float, 3> pvCoord = {collision.posX(), collision.posY(), collision.posZ()};
      double cpaLambda = casc.v0cosPA(collision.posX(), collision.posY(), collision.posZ());
      double cpaXYLambda = RecoDecay::cpaXY(pvCoord, vertexV0, pVecV0);
      double cpaXi = casc.casccosPA(collision.posX(), collision.posY(), collision.posZ());
      double cpaXYXi = RecoDecay::cpaXY(pvCoord, vertexCasc, pVecCasc);

      // get DCAs of Pi0-Pi1, Pi0-Xi, Pi1-Xi
      float dcaXYPi0Pi1 = kfCharmBachelor0.GetDistanceFromParticleXY(kfCharmBachelor1);
      float dcaXYPi0Xi = kfCharmBachelor0.GetDistanceFromParticleXY(kfXi);
      float dcaXYPi1Xi = kfCharmBachelor1.GetDistanceFromParticleXY(kfXi);

      // mass of Xi-Pi0 pair
      KFParticle kfXiPi0;
      const KFParticle* kfXiResonanceDaughtersPi0[2] = {&kfXi, &kfCharmBachelor0};
      kfXiPi0.SetConstructMethod(kfConstructMethod);
      try {
        kfXiPi0.Construct(kfXiResonanceDaughtersPi0, 2);
        massXiPi0 = kfXiPi0.GetMass();
      } catch(...) {
        LOG(info) << "Failed to construct Xi(1530) with Pi 0";
      }

      // mass of Xi-Pi1 pair
      KFParticle kfXiPi1;
      const KFParticle* kfXiResonanceDaughtersPi1[2] = {&kfXi, &kfCharmBachelor1};
      kfXiPi1.SetConstructMethod(kfConstructMethod);
      try {
        kfXiPi1.Construct(kfXiResonanceDaughtersPi1, 2);
        massXiPi1 = kfXiPi1.GetMass();
      } catch(...){
        LOG(info) << "Failed to construct Xi(1530) with Pi 1";
      }

      //-------------------------------fill histograms--------------------------------------------
      if (fillHistograms) {
        // invariant mass
        hMass3->Fill(kfXicPlus.GetMass());
        // covariance matrix elements of PV
        hCovPVXX->Fill(covMatrixPV[0]);
        hCovPVYY->Fill(covMatrixPV[2]);
        hCovPVXZ->Fill(covMatrixPV[3]);
        hCovPVZZ->Fill(covMatrixPV[5]);
        // covariance matrix elements of SV
        hCovSVXX->Fill(covMatrixXicPlus[0]);
        hCovSVYY->Fill(covMatrixXicPlus[2]);
        hCovSVXZ->Fill(covMatrixXicPlus[3]);
        hCovSVZZ->Fill(covMatrixXicPlus[5]);
        // DCAs of prongs
        hDcaXYProngs->Fill(kfCharmBachelor0.GetPt(), impactParameterPi0XY);
        hDcaXYProngs->Fill(kfCharmBachelor1.GetPt(), impactParameterPi1XY);
        hDcaXYProngs->Fill(kfXi.GetPt(), impactParameterXiXY);
      }

      //------------------------------fill candidate table rows--------------------------------------
      rowCandidateBase(collision.globalIndex(),
                       KFPV.GetX(), KFPV.GetY(), KFPV.GetZ(),
                       covMatrixPV[0], covMatrixPV[2], covMatrixPV[5],
                       /*3-prong specific columns*/
                       rowTrackIndexXicPlus.cascadeId(), rowTrackIndexXicPlus.prong0Id(), rowTrackIndexXicPlus.prong1Id(),
                       casc.bachelorId(), casc.posTrackId(), casc.negTrackId(),
                       kfXicPlus.GetX(), kfXicPlus.GetY(), kfXicPlus.GetZ(),
                       kfXicPlus.GetErrX(), kfXicPlus.GetErrY(), kfXicPlus.GetErrZ(),
                       kfXicPlus.GetErrDecayLength(), kfXicPlus.GetErrDecayLengthXY(),
                       kfXicPlus.GetChi2(), kfXicPlus.GetMass(), signXic,
                       kfXi.GetPx(), kfXi.GetPy(), kfXi.GetPz(),
                       kfCharmBachelor0.GetPx(), kfCharmBachelor0.GetPy(), kfCharmBachelor0.GetPz(),
                       kfCharmBachelor1.GetPx(), kfCharmBachelor1.GetPy(), kfCharmBachelor1.GetPz(),
                       impactParameterXiXY, impactParameterPi0XY, impactParameterPi1XY,
                       errImpactParameterXiXY, errImpactParameterPi0XY, errImpactParameterPi1XY,
                       hfFlag,
                       /*cascade specific columns*/
                       casc.x(), casc.y(), casc.z(),
                       casc.xlambda(), casc.ylambda(), casc.zlambda(),
                       cpaXi, cpaXYXi, cpaLambda, cpaXYLambda,
                       massXiPi0, massXiPi1);
      rowCandidateKF(casc.kfCascadeChi2(), casc.kfV0Chi2(),
                     dcaXYPi0Pi1, dcaXYPi0Xi, dcaXYPi1Xi);
    } // loop over track triplets
  }
  PROCESS_SWITCH(HfCandidateCreatorXic, processXicplusWithKFParticleFromDerivedData, "Run candidate creator with KFParticle using derived data from HfTrackIndexSkimCreatorLfCascades.", false);

  void processXicplusWithKFParticle(SelectedCollisions const& collisions,
                                    SelectedHfTrackAssoc const& trackIndices,
                                    KFCascadesLinked const& linkedCascades,
                                    KFCascFull const&,
                                    aod::TracksWCovExtra const&,
                                    aod::BCsWithTimestamps const&)
  {
    for (const auto& collision : collisions) {
      //------------------------- Set the magnetic field from ccdb---------------------------------
      /// The static instance of the propagator was already modified in the HFTrackIndexSkimCreator,
      /// but this is not true when running on Run2 data/MC already converted into AO2Ds.
      auto bc = collision.bc_as<aod::BCsWithTimestamps>();
      if (runNumber != bc.runNumber()) {
        LOG(info) << ">>>>>>>>>>>> Current run number: " << runNumber;
        initCCDB(bc, runNumber, ccdb, isRun2 ? ccdbPathGrp : ccdbPathGrpMag, lut, isRun2);
        bz = o2::base::Propagator::Instance()->getNominalBz();
        LOG(info) << ">>>>>>>>>>>> Magnetic field: " << bz;
      }
      KFParticle::SetField(bz);

      // ------------------------------------cascade loop-------------------------------------------
      auto thisCollId = collision.globalIndex();
      // auto groupedCascades = cascades.sliceBy(cascadesPerCollision, thisCollId);
      auto groupedLinkedCascades = linkedCascades.sliceBy(linkedCascadesPerCollision, thisCollId);
      for (const auto& linkedCasc : groupedLinkedCascades) {
        if (!linkedCasc.has_kfCascData()) {
          continue;
        }
        auto casc = linkedCasc.kfCascData_as<KFCascFull>();

        //----------------accessing particles in the decay chain-------------
        // cascade daughter - charged particle
        auto trackCascDauCharged = casc.bachelor_as<aod::TracksWCovExtra>(); // meson <- xi track
        // cascade daughter - V0
        auto trackV0PosDau = casc.posTrack_as<aod::TracksWCovExtra>(); // p <- V0 track (positive track) 0
        // V0 negative daughter
        auto trackV0NegDau = casc.negTrack_as<aod::TracksWCovExtra>(); // pion <- V0 track (negative track) 1

        // check that particles come from the same collision
        if (rejDiffCollTrack) {
          if (trackV0PosDau.collisionId() != trackV0NegDau.collisionId()) {
            continue;
          }
          if (trackCascDauCharged.collisionId() != trackV0PosDau.collisionId()) {
            continue;
          }
        }
        // check not to take cascade daughters twice
        if (trackV0PosDau.globalIndex() == trackV0NegDau.globalIndex() || trackV0PosDau.globalIndex() == trackCascDauCharged.globalIndex() || trackV0NegDau.globalIndex() == trackCascDauCharged.globalIndex()) {
          continue;
        }

        // preselect cascade candidates
        if (doCascadePreselection) {
          if (std::abs(casc.dcaXYCascToPV()) > dcaXYToPVCascadeMax) {
            continue;
          }
          if (std::abs(casc.mXi() - massXiMinusFromPdg) > massToleranceCascade) {
            continue;
          }
        }

        //--------------------------------------loop over first bachelor-----------------------------------------------------
        auto groupedBachTrackIndices = trackIndices.sliceBy(trackIndicesPerCollision, thisCollId);
        for (auto trackIdCharmBachelor0 = groupedBachTrackIndices.begin(); trackIdCharmBachelor0 != groupedBachTrackIndices.end(); ++trackIdCharmBachelor0) {
          auto trackCharmBachelor0 = trackIdCharmBachelor0.track_as<aod::TracksWCovExtra>();

          // check that particles come from the same collision
          if ((rejDiffCollTrack) && (trackCascDauCharged.collisionId() != trackCharmBachelor0.collisionId())) {
            continue;
          }
          // ask for opposite sign daughters
          if (trackCharmBachelor0.sign() * trackCascDauCharged.sign() >= 0) {
            continue;
          }
          // check not to take the same particle twice in the decay chain
          if (trackCharmBachelor0.globalIndex() == trackCascDauCharged.globalIndex() || trackCharmBachelor0.globalIndex() == trackV0PosDau.globalIndex() || trackCharmBachelor0.globalIndex() == trackV0NegDau.globalIndex()) {
            continue;
          }

          //-----------------------------------------------loop over second bachelor---------------------------------------------------------------------------
          for (auto trackIdCharmBachelor1 = trackIdCharmBachelor0 + 1; trackIdCharmBachelor1 != groupedBachTrackIndices.end(); ++trackIdCharmBachelor1) {
            auto trackCharmBachelor1 = trackIdCharmBachelor1.track_as<aod::TracksWCovExtra>();
            // check that particles come from the same collision
            if ((rejDiffCollTrack) && (trackCascDauCharged.collisionId() != trackCharmBachelor1.collisionId())) {
              continue;
            }
            // ask for same sign daughters
            if (trackCharmBachelor1.sign() * trackCharmBachelor0.sign() <= 0) {
              continue;
            }
            // check not to take the same particle twice in the decay chain
            if (trackCharmBachelor1.globalIndex() == trackCharmBachelor0.globalIndex() || trackCharmBachelor1.globalIndex() == trackCascDauCharged.globalIndex() || trackCharmBachelor1.globalIndex() == trackV0PosDau.globalIndex() || trackCharmBachelor1.globalIndex() == trackV0NegDau.globalIndex()) {
              continue;
            }

            //----------------------info of V0 and cascade tracks from LF-table------------------
            std::array<float, 3> vertexV0 = {casc.xlambda(), casc.ylambda(), casc.zlambda()};
            std::array<float, 3> pVecV0 = {casc.pxlambda(), casc.pylambda(), casc.pzlambda()};
            std::array<float, 3> vertexCasc = {casc.x(), casc.y(), casc.z()};
            std::array<float, 3> pVecCasc = {casc.px(), casc.py(), casc.pz()};

            //----------------------Create XicPlus as KFParticle object-------------------------------------------
            // initialize primary vertex
            KFPVertex kfpVertex = createKFPVertexFromCollision(collision);
            float covMatrixPV[6];
            kfpVertex.GetCovarianceMatrix(covMatrixPV);
            KFParticle KFPV(kfpVertex); // for calculation of DCAs to PV

            // convert pion tracks into KFParticle object
            KFPTrack kfpTrackCharmBachelor0 = createKFPTrackFromTrack(trackCharmBachelor0);
            KFPTrack kfpTrackCharmBachelor1 = createKFPTrackFromTrack(trackCharmBachelor1);
            KFParticle kfCharmBachelor0(kfpTrackCharmBachelor0, kPiPlus);
            KFParticle kfCharmBachelor1(kfpTrackCharmBachelor1, kPiPlus);

            // create Xi as KFParticle object
            // read {X,Y,Z,Px,Py,Pz} and corresponding covariance matrix from KF cascade Tables
            std::array<float, 6> xyzpxpypz = {casc.x(), casc.y(), casc.z(), casc.px(), casc.py(), casc.pz()};
            float parPosMom[6];
            for (int i{0}; i < 6; ++i) {
              parPosMom[i] = xyzpxpypz[i];
            }
            // create KFParticle
            KFParticle kfXi;
            kfXi.Create(parPosMom, casc.kfTrackCovMat(), casc.sign(), casc.mXi());

            // create XicPlus as KFParticle object
            KFParticle kfXicPlus;
            const KFParticle* kfDaughtersXicPlus[3] = {&kfCharmBachelor0, &kfCharmBachelor1, &kfXi};
            kfXicPlus.SetConstructMethod(kfConstructMethod);
            try {
              kfXicPlus.Construct(kfDaughtersXicPlus, 3);
            } catch (std::runtime_error& e) {
              LOG(debug) << "Failed to construct XicPlus : " << e.what();
              continue;
            }

            // topological constraint
            if (constrainXicPlusToPv) {
              kfXicPlus.SetProductionVertex(KFPV);
            }
            auto covMatrixXicPlus = kfXicPlus.CovarianceMatrix();

            // transport daughter particles to XicPlus decay vertex
            kfCharmBachelor0.TransportToParticle(kfXicPlus);
            kfCharmBachelor1.TransportToParticle(kfXicPlus);
            kfXi.TransportToParticle(kfXicPlus);

            //---------------------calculate physical parameters of XicPlus candidate----------------------
            // set hfFlag
            int hfFlag = BIT(aod::hf_cand_xictoxipipi::DecayType::XicToXiPiPi);
            
            // sign of charm baryon
            int signXic = casc.sign() < 0 ? +1 : -1;

            // get impact parameters of XicPlus daughters
            float impactParameterPi0XY = 0., errImpactParameterPi0XY = 0.;
            float impactParameterPi1XY = 0., errImpactParameterPi1XY = 0.;
            float impactParameterXiXY = 0., errImpactParameterXiXY = 0.;
            kfCharmBachelor0.GetDistanceFromVertexXY(KFPV, impactParameterPi0XY, errImpactParameterPi0XY);
            kfCharmBachelor1.GetDistanceFromVertexXY(KFPV, impactParameterPi1XY, errImpactParameterPi1XY);
            kfXi.GetDistanceFromVertexXY(KFPV, impactParameterXiXY, errImpactParameterXiXY);

            // calculate cosine of pointing angle
            std::array<float, 3> pvCoord = {collision.posX(), collision.posY(), collision.posZ()};
            double cpaLambda = casc.v0cosPA(collision.posX(), collision.posY(), collision.posZ());
            double cpaXYLambda = RecoDecay::cpaXY(pvCoord, vertexV0, pVecV0);
            double cpaXi = casc.casccosPA(collision.posX(), collision.posY(), collision.posZ());
            double cpaXYXi = RecoDecay::cpaXY(pvCoord, vertexCasc, pVecCasc);

            // get DCAs of Pi0-Pi1, Pi0-Xi, Pi1-Xi
            float dcaXYPi0Pi1 = kfCharmBachelor0.GetDistanceFromParticleXY(kfCharmBachelor1);
            float dcaXYPi0Xi = kfCharmBachelor0.GetDistanceFromParticleXY(kfXi);
            float dcaXYPi1Xi = kfCharmBachelor1.GetDistanceFromParticleXY(kfXi);

            // mass of Xi-Pi0 pair
            KFParticle kfXiPi0;
            const KFParticle* kfXiResonanceDaughtersPi0[2] = {&kfXi, &kfCharmBachelor0};
            kfXiPi0.SetConstructMethod(kfConstructMethod);
            try {
              kfXiPi0.Construct(kfXiResonanceDaughtersPi0, 2);
              massXiPi0 = kfXiPi0.GetMass();
            } catch(...) {
              LOG(info) << "Failed to construct Xi(1530) with Pi 0";
            }

            // mass of Xi-Pi1 pair
            KFParticle kfXiPi1;
            const KFParticle* kfXiResonanceDaughtersPi1[2] = {&kfXi, &kfCharmBachelor1};
            kfXiPi1.SetConstructMethod(kfConstructMethod);
            try {
              kfXiPi1.Construct(kfXiResonanceDaughtersPi1, 2);
              massXiPi1 = kfXiPi1.GetMass();
            } catch(...){
              LOG(info) << "Failed to construct Xi(1530) with Pi 1";
            }

            //-------------------------------fill histograms--------------------------------------------
            if (fillHistograms) {
              // invariant mass
              hMass3->Fill(kfXicPlus.GetMass());
              // covariance matrix elements of PV
              hCovPVXX->Fill(covMatrixPV[0]);
              hCovPVYY->Fill(covMatrixPV[2]);
              hCovPVXZ->Fill(covMatrixPV[3]);
              hCovPVZZ->Fill(covMatrixPV[5]);
              // covariance matrix elements of SV
              hCovSVXX->Fill(covMatrixXicPlus[0]);
              hCovSVYY->Fill(covMatrixXicPlus[2]);
              hCovSVXZ->Fill(covMatrixXicPlus[3]);
              hCovSVZZ->Fill(covMatrixXicPlus[5]);
              // DCAs of prongs
              hDcaXYProngs->Fill(kfCharmBachelor0.GetPt(), impactParameterPi0XY);
              hDcaXYProngs->Fill(kfCharmBachelor1.GetPt(), impactParameterPi1XY);
              hDcaXYProngs->Fill(kfXi.GetPt(), impactParameterXiXY);
            }

            //------------------------------fill candidate table rows--------------------------------------
            rowCandidateBase(collision.globalIndex(),
                             KFPV.GetX(), KFPV.GetY(), KFPV.GetZ(),
                             covMatrixPV[0], covMatrixPV[2], covMatrixPV[5],
                             /*3-prong specific columns*/
                             casc.cascadeId(), trackCharmBachelor0.globalIndex(), trackCharmBachelor1.globalIndex(),
                             casc.bachelorId(), casc.posTrackId(), casc.negTrackId(),
                             kfXicPlus.GetX(), kfXicPlus.GetY(), kfXicPlus.GetZ(),
                             kfXicPlus.GetErrX(), kfXicPlus.GetErrY(), kfXicPlus.GetErrZ(),
                             kfXicPlus.GetErrDecayLength(), kfXicPlus.GetErrDecayLengthXY(),
                             kfXicPlus.GetChi2(), kfXicPlus.GetMass(), signXic,
                             kfXi.GetPx(), kfXi.GetPy(), kfXi.GetPz(),
                             kfCharmBachelor0.GetPx(), kfCharmBachelor0.GetPy(), kfCharmBachelor0.GetPz(),
                             kfCharmBachelor1.GetPx(), kfCharmBachelor1.GetPy(), kfCharmBachelor1.GetPz(),
                             impactParameterXiXY, impactParameterPi0XY, impactParameterPi1XY,
                             errImpactParameterXiXY, errImpactParameterPi0XY, errImpactParameterPi1XY,
                             hfFlag,
                             /*cascade specific columns*/
                             casc.x(), casc.y(), casc.z(),
                             casc.xlambda(), casc.ylambda(), casc.zlambda(),
                             cpaXi, cpaXYXi, cpaLambda, cpaXYLambda,
                             massXiPi0, massXiPi1);
            rowCandidateKF(casc.kfCascadeChi2(), casc.kfV0Chi2(),
                           dcaXYPi0Pi1, dcaXYPi0Xi, dcaXYPi1Xi);
          } // bachelor 1
        } // bachelor 0
      } // cascades
    } // collisions
  }
  PROCESS_SWITCH(HfCandidateCreatorXic, processXicplusWithKFParticle, "Run candidate creator with KFParticle", false);
}; // struct

/// Performs MC matching.
struct HfCandidateCreatorXicExpressions {
  Spawns<aod::HfCandXicExt> rowCandidateXic;
  Produces<aod::HfCandXicMcRec> rowMcMatchRec;
  Produces<aod::HfCandXicMcGen> rowMcMatchGen;

  void init(InitContext const&) {}

  void processMc(aod::TracksWMc const& tracks,
                 aod::McParticles const& mcParticles)
  {
    rowCandidateXic->bindExternalIndices(&tracks);

    int indexRec = -1;
    int indexRecXicPlus = -1;
    int indexRes = -1;
    int8_t sign = -9;
    int8_t flag = 0;
    int8_t origin = 0;
    int8_t debug = 0;

    int pdgCodeXicPlus = Pdg::kXiCPlus; // 4232
    int pdgCodeXiMinus = kXiMinus;      // 3312
    int pdgCodeXiRes = 3324;            // 3324
    int pdgCodeLambda = kLambda0;       // 3122
    int pdgCodePiPlus = kPiPlus;        // 211
    int pdgCodePiMinus = kPiMinus;      // -211
    int pdgCodeProton = kProton;        // 2212

    // Match reconstructed candidates.
    for (const auto& candidate : *rowCandidateXic) {
      flag = 0;
      sign = -9;
      origin = RecoDecay::OriginType::None;
      debug = 0;

      auto arrayDaughters = std::array{candidate.pi0_as<aod::TracksWMc>(),       // pi <- Xic
                                       candidate.pi1_as<aod::TracksWMc>(),       // pi <- Xic
                                       candidate.bachelor_as<aod::TracksWMc>(),  // pi <- cascade
                                       candidate.posTrack_as<aod::TracksWMc>(),  // p <- lambda
                                       candidate.negTrack_as<aod::TracksWMc>()}; // pi <- lambda
      auto arrayDaughtersResPi0 = std::array{candidate.cascade_as<aod::TracksWMc>(),
                                             candidate.pi0_as<aod::TracksWMc>()};
      auto arrayDaughtersResPi1 = std::array{candidate.cascade_as<aod::TracksWMc>(),
                                             candidate.pi1_as<aod::TracksWMc>()};
      auto arrayDaughtersCasc = std::array{candidate.bachelor_as<aod::TracksWMc>(),
                                           candidate.posTrack_as<aod::TracksWMc>(),
                                           candidate.negTrack_as<aod::TracksWMc>()};
      auto arrayDaughtersV0 = std::array{candidate.posTrack_as<aod::TracksWMc>(),
                                         candidate.negTrack_as<aod::TracksWMc>()};

      // Xic → pi pi pi pi p
      indexRec = RecoDecay::getMatchedMCRec(mcParticles, arrayDaughters, pdgCodeXicPlus, std::array{pdgCodePiPlus, pdgCodePiPlus, pdgCodePiMinus, pdgCodeProton, pdgCodePiMinus}, true, &sign, 4);
      indexRecXicPlus = indexRec;
      if (indexRec == -1) {
        debug = 1;
      }
      if (indexRec > -1) {
        // Xi- → pi pi p
        indexRec = RecoDecay::getMatchedMCRec(mcParticles, arrayDaughtersCasc, pdgCodeXiMinus, std::array{pdgCodePiMinus, pdgCodeProton, pdgCodePiMinus}, true, &sign, 2);
        if (indexRec == -1) {
          debug = 2;
        }
        if (indexRec > -1) {
          // Lambda → p pi
          indexRec = RecoDecay::getMatchedMCRec(mcParticles, arrayDaughtersV0, pdgCodeLambda, std::array{pdgCodeProton, pdgCodePiMinus}, true, &sign, 1);
          if (indexRec == -1) {
            debug = 3;
          }
          if (indexRec > -1) {
            // Xic → Xi(1530) pi
            indexRes = RecoDecay::getMatchedMCRec(mcParticles, arrayDaughtersResPi0, pdgCodeXicPlus, std::array{pdgCodeXiRes, pdgCodePiMinus}, true, &sign, 1);
            if (indexRes > -1) {
              flag = sign * (1 << aod::hf_cand_xictoxipipi::DecayType::XicToXiResPiToXiPiPi);
            } else if (indexRes == -1) {
              indexRes = RecoDecay::getMatchedMCRec(mcParticles, arrayDaughtersResPi1, pdgCodeXicPlus, std::array{pdgCodeXiRes, pdgCodePiMinus}, true, &sign, 1);
              if (indexRes > -1) {
                flag = sign * (1 << aod::hf_cand_xictoxipipi::DecayType::XicToXiResPiToXiPiPi);
              } else if (indexRes == -1) {
                flag = sign * (1 << aod::hf_cand_xictoxipipi::DecayType::XicToXiPiPi);
              }
            }
          }
        }
      }

      // Check whether the charm baryon is non-prompt (from a b quark).
      if (flag != 0) {
        auto particle = mcParticles.rawIteratorAt(indexRecXicPlus);
        origin = RecoDecay::getCharmHadronOrigin(mcParticles, particle, true);
      }

      rowMcMatchRec(flag, debug, origin);
    } // close loop over candidates

    // Match generated particles.
    for (const auto& particle : mcParticles) {
      flag = 0;
      sign = -9;
      debug = 0;
      origin = RecoDecay::OriginType::None;

      //  Xic → Xi pi pi
      if (RecoDecay::isMatchedMCGen(mcParticles, particle, pdgCodeXicPlus, std::array{pdgCodeXiMinus, pdgCodePiPlus, pdgCodePiPlus}, true, &sign, 2)) {
        debug = 1;
        // Xi- -> Lambda pi
        auto cascMC = mcParticles.rawIteratorAt(particle.daughtersIds().front());
        if (RecoDecay::isMatchedMCGen(mcParticles, cascMC, pdgCodeXiMinus, std::array{pdgCodeLambda, pdgCodePiMinus}, true)) {
          debug = 2;
          // Lambda -> p pi
          auto v0MC = mcParticles.rawIteratorAt(cascMC.daughtersIds().front());
          if (RecoDecay::isMatchedMCGen(mcParticles, v0MC, pdgCodeLambda, std::array{pdgCodeProton, pdgCodePiMinus}, true)) {
            debug = 3;
            if (RecoDecay::isMatchedMCGen(mcParticles, particle, pdgCodeXicPlus, std::array{pdgCodeXiRes, pdgCodePiMinus}, true)) {
              flag = sign * (1 << aod::hf_cand_xictoxipipi::DecayType::XicToXiResPiToXiPiPi);
            } else {
              flag = sign * (1 << aod::hf_cand_xictoxipipi::DecayType::XicToXiPiPi);
            }
          }
        }
      }

      // Check whether the charm baryon is non-prompt (from a b quark).
      if (flag != 0) {
        origin = RecoDecay::getCharmHadronOrigin(mcParticles, particle, true);
      }

      rowMcMatchGen(flag, debug, origin);
    } // close loop over generated particles
  } // close process
  PROCESS_SWITCH(HfCandidateCreatorXicExpressions, processMc, "Process MC", false);
}; // close struct

WorkflowSpec defineDataProcessing(ConfigContext const& cfgc)
{
  return WorkflowSpec{
    adaptAnalysisTask<HfCandidateCreatorXic>(cfgc),
    adaptAnalysisTask<HfCandidateCreatorXicExpressions>(cfgc)};
}
