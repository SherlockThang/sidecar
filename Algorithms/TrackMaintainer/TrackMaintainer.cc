#include "boost/bind.hpp"

#include "Algorithms/Controller.h"
#include "Logger/Log.h"
#include "Messages/RadarConfig.h"
#include "Messages/TSPI.h"
#include "Messages/Track.h"
#include "Time/TimeStamp.h"

#include "TrackMaintainer.h"
#include "TrackMaintainer_defaults.h"
#include "Utils/Utils.h"

#include "QtCore/QString"
#include "QtCore/QVariant"

using namespace SideCar;
using namespace SideCar::Messages;
using namespace SideCar::Algorithms;

TrackMaintainer::TrackMaintainer(Controller& controller, Logger::Log& log) :
    Super(controller, log), enabled_(Parameter::BoolValue::Make("enabled", "Enabled", kDefaultEnabled)),
    hitsBeforePromote_(Parameter::PositiveIntValue::Make("hitsBeforePromote",
                                                         "The number of detections associated with a "
                                                         "track before it is promoted to firm",
                                                         kDefaultHitsBeforePromote)),
    missesBeforeDrop_(Parameter::PositiveIntValue::Make("missesBeforeDrop",
                                                        "The number of missed scans allowed before a "
                                                        "track is dropped",
                                                        kDefaultMissesBeforeDrop))
{
    ;
}

bool
TrackMaintainer::startup()
{
    registerProcessor<TrackMaintainer, Track>("corrected", &TrackMaintainer::processInput);
    setAlarm(5); // set an alarm to go off every 5 seconds
    return registerParameter(enabled_) && registerParameter(missesBeforeDrop_) &&
           registerParameter(hitsBeforePromote_) && Super::startup();
}

bool
TrackMaintainer::reset()
{
    trackDatabase_.clear();
    return Super::reset();
}

/** This method will get called whenever the timer for this Algorithm goes off
 */
void
TrackMaintainer::processAlarm()
{
    static Logger::ProcLog log("processAlarm", getLog());
    LOGINFO << "checking database - trks " << trackDatabase_.size() << std::endl;
    checkDatabase();
}

bool
TrackMaintainer::shutdown()
{
    trackDatabase_.clear();
    return Super::shutdown();
}

bool
TrackMaintainer::processInput(const Messages::Track::Ref& msg)
{
    static Logger::ProcLog log("processInput", getLog());

    LOGDEBUG << msg->headerPrinter() << std::endl;
    LOGDEBUG << msg->dataPrinter() << std::endl;

    if (msg->getFlags() == Track::kNew || msg->getFlags() == Track::kCorrected) { updateDatabase(msg); }

    return true;
}

void
TrackMaintainer::updateDatabase(const Messages::Track::Ref& msg)
{
    static Logger::ProcLog log("updateDatabase", getLog());
    LOGDEBUG << "track database has " << trackDatabase_.size() << " entries" << std::endl;

    // Find existing entry in map or create a new one
    //
    Mapping::iterator it = trackDatabase_.find(msg->getTrackNumber());
    if (it == trackDatabase_.end()) {
        LOGDEBUG << "new entry for track num " << msg->getTrackNumber() << std::endl;
        it = trackDatabase_.insert(Mapping::value_type(msg->getTrackNumber(), TrackMsgVector())).first;
    }

    // Add the given message to the track vector
    //
    it->second.push_back(msg);

    // Update the offset between message time and wall time.
    //
    Time::TimeStamp now = Time::TimeStamp::Now();
    epoch_ = now.asDouble() - msg->getExtractionTime();

    LOGDEBUG << "epoch " << epoch_ << std::endl;
}

/** This method checks for tracks that need to be promoted from tentative to firm and also for tracks that have
    not been updated recently and need to be dropped. If a track needs to be promoted or dropped, this method
    will send out a Track message to this effect.

    This method should be careful not to use current time to compare against
    because we need to be able to accomodate data playback, which may contain
    data with timestamps that are far in the past.
*/

void
TrackMaintainer::checkDatabase()
{
    static Logger::ProcLog log("checkDatabase", getLog());

    LOGINFO << "track database has " << trackDatabase_.size() << " entries" << std::endl;

    double dropLimit = RadarConfig::GetRotationDuration() * missesBeforeDrop_->getValue();
    LOGDEBUG << "drop duration " << dropLimit << std::endl;

    // Loop through the data base.
    //
    Mapping::iterator it = trackDatabase_.begin();
    while (it != trackDatabase_.end()) {
        LOGDEBUG << "track database entry " << it->first << std::endl;

        const TrackMsgVector& tracks(it->second);
        const Messages::Track::Ref& track(tracks.back());

        Messages::Track::Ref report;

        // Check to see if track is promotable
        //
        if (track->getType() == Track::kTentative) {
            LOGDEBUG << "tentative track with " << tracks.size()
                     << " msgs. Threshold= " << hitsBeforePromote_->getValue() << std::endl;
            if (tracks.size() >= hitsBeforePromote_->getValue()) {
                LOGDEBUG << "track should be promoted " << std::endl;
                report = Track::Make("TrackMaintainer", track);
                report->setFlags(Track::kPromoted);
            }
        }

        // Check to see if track should be dropped
        //
        double now = Time::TimeStamp::Now().asDouble();
        if ((now - (epoch_ + track->getExtractionTime())) > dropLimit) {
            LOGWARNING << "dropping track " << it->first << std::endl;

            report = Track::Make("TrackMaintainer", track);
            report->setFlags(Track::kDropping);

            // NOTE: erasing the current item only affects the current iterator but no others, so we
            // post-increment so that we have a valid iterator to the next element after the erasure.
            //
            trackDatabase_.erase(it++);
        } else {
            // Not deleting track, so safe to advance the iterator to the next one.
            //
            ++it;
        }

        // If we created a report above, send it out.
        //
        if (report) {
            send(report);

            std::string flag;
            switch (report->getFlags()) {
            case Track::kDropping: flag = " dropping"; break;
            case Track::kNew: flag = "new"; break;
            case Track::kPromoted: flag = "promoted"; break;
            case Track::kNeedsPrediction: flag = "needs prediction"; break;
            case Track::kNeedsCorrection: flag = "needs correction"; break;
            case Track::kPredicted: flag = "predicted"; break;
            case Track::kCorrected: flag = "corrected"; break;
            default: break;
            };

            std::string type;
            switch (report->getType()) {
            case Track::kTentative: type = "tentative"; break;
            case Track::kConfirmed: type = "confirmed"; break;
            default: break;
            };

            LOGDEBUG << "maintained track: " << report->getTrackNumber() << " flag: " << flag << " type: " << type
                     << std::endl;
        }
    }
}

void
TrackMaintainer::setInfoSlots(IO::StatusBase& status)
{
    status.setSlot(kEnabled, enabled_->getValue());
}

extern "C" ACE_Svc_Export QVariant
FormatInfo(const IO::StatusBase& status, int role)
{
    if (role != Qt::DisplayRole) return QVariant();

    if (!status[TrackMaintainer::kEnabled])
        return "Disabled";
    else
        return "Enabled";
}

// Factory function for the DLL that will create a new instance of the ABTracker class. DO NOT CHANGE!
//
extern "C" ACE_Svc_Export Algorithm*
TrackMaintainerMake(Controller& controller, Logger::Log& log)
{
    return new TrackMaintainer(controller, log);
}
