#include "BYTETracker.h"

BYTETracker::BYTETracker(int frame_rate, int track_buffer) {
    track_thresh  = 0.1; //low-confidence selection threshold, below which the detection box is not tracked.
    high_thresh   = 0.75; 
    match_thresh  = 0.5; //matching threshold for the IOU of the previous frame and the second frame, if there is a (1-IOU) > match_thresh, the direct tracking fails.
    frame_id      = 0;
    max_time_lost = 5;
}

BYTETracker::~BYTETracker() {
}

vector<STrack> BYTETracker::update(const vector<NvObject> &nvObjects) {

    ////////////////// Step 1: Get detections //////////////////
    this->frame_id++;
    vector<STrack> activated_stracks;
    vector<STrack> refind_stracks;
    vector<STrack> removed_stracks;
    vector<STrack> lost_stracks;
    vector<STrack> detections;
    vector<STrack> detections_low;

    vector<STrack> detections_cp;
    vector<STrack> tracked_stracks_swap;
    vector<STrack> resa, resb;
    vector<STrack> output_stracks;

    vector<STrack *> unconfirmed;
    vector<STrack *> tracked_stracks;
    vector<STrack *> strack_pool;
    vector<STrack *> r_tracked_stracks;


    if (nvObjects.size() > 0) {
        for (int i = 0; i < nvObjects.size(); i++) {
            vector<float> tlwh_;
            tlwh_.resize(4);
            tlwh_[0] = nvObjects[i].rect[0];
            tlwh_[1] = nvObjects[i].rect[1];
            tlwh_[2] = nvObjects[i].rect[2];
            tlwh_[3] = nvObjects[i].rect[3];
            float score = nvObjects[i].prob;

            STrack strack(tlwh_, score, nvObjects[i].label, nvObjects[i].associatedObjectIn);
            if (score >= track_thresh) {
                detections.push_back(strack);
            } else {
                detections_low.push_back(strack);
            }
        }
    }

    // Add newly detected tracklets to tracked_stracks
    for (int i = 0; i < this->tracked_stracks.size(); i++) {
        if (!this->tracked_stracks[i].is_activated)
            unconfirmed.push_back(&this->tracked_stracks[i]);
        else
            tracked_stracks.push_back(&this->tracked_stracks[i]);
    }


    ////////////////// Step 2: First association, with IoU //////////////////
    strack_pool                     = joint_stracks(tracked_stracks, this->lost_stracks);
    STrack::multi_predict(strack_pool, this->kalman_filter);

    vector<vector<float>> dists;
    int                   dist_size = 0, dist_size_size = 0;
    dists = iou_distance(strack_pool, detections, dist_size, dist_size_size);

    vector<vector<int>> matches;
    vector<int>         u_track, u_detection;
    linear_assignment(dists, dist_size, dist_size_size, match_thresh, matches, u_track, u_detection);

    for (int i = 0; i < matches.size(); i++) {
        STrack *track = strack_pool[matches[i][0]];
        STrack *det   = &detections[matches[i][1]];
        if (track->state == TrackState::Tracked) {
            track->update(*det, this->frame_id);
            activated_stracks.push_back(*track);
        } else {
            track->re_activate(*det, this->frame_id, false);
            refind_stracks.push_back(*track);
        }
    }

    ////////////////// Step 3: Second association, using low score dets //////////////////
    for (int i = 0; i < u_detection.size(); i++) {
        detections_cp.push_back(detections[u_detection[i]]);
    }
    detections.clear();
    detections.assign(detections_low.begin(), detections_low.end());

    for (int i = 0; i < u_track.size(); i++) {
        if (strack_pool[u_track[i]]->state == TrackState::Tracked) {
            r_tracked_stracks.push_back(strack_pool[u_track[i]]);
        }
    }

    dists.clear();
    dists = iou_distance(r_tracked_stracks, detections, dist_size, dist_size_size);

    matches.clear();
    u_track.clear();
    u_detection.clear();
    linear_assignment(dists, dist_size, dist_size_size, 0.5, matches, u_track, u_detection);

    for (int i = 0; i < matches.size(); i++) {
        STrack *track = r_tracked_stracks[matches[i][0]];
        STrack *det   = &detections[matches[i][1]];
        if (track->state == TrackState::Tracked) {
            track->update(*det, this->frame_id);
            activated_stracks.push_back(*track);
        } else {
            track->re_activate(*det, this->frame_id, false);
            refind_stracks.push_back(*track);
        }
    }

    for (int i = 0; i < u_track.size(); i++) {
        STrack *track = r_tracked_stracks[u_track[i]];
        if (track->state != TrackState::Lost) {
            track->mark_lost();
            lost_stracks.push_back(*track);
        }
    }

    // Deal with unconfirmed tracks, usually tracks with only one beginning frame
    detections.clear();
    detections.assign(detections_cp.begin(), detections_cp.end());

    dists.clear();
    dists = iou_distance(unconfirmed, detections, dist_size, dist_size_size);

    matches.clear();
    vector<int> u_unconfirmed;
    u_detection.clear();
    linear_assignment(dists, dist_size, dist_size_size, 0.7, matches, u_unconfirmed, u_detection);

    for (int i = 0; i < matches.size(); i++) {
        unconfirmed[matches[i][0]]->update(detections[matches[i][1]], this->frame_id);
        activated_stracks.push_back(*unconfirmed[matches[i][0]]);
    }

    for (int i = 0; i < u_unconfirmed.size(); i++) {
        STrack *track = unconfirmed[u_unconfirmed[i]];
        track->mark_removed();
        removed_stracks.push_back(*track);
    }

    ////////////////// Step 4: Init new stracks //////////////////
    for (int i = 0; i < u_detection.size(); i++) {
        STrack *track = &detections[u_detection[i]];
        if (track->score < this->high_thresh)
            continue;
        track->activate(this->kalman_filter, this->frame_id);
        activated_stracks.push_back(*track);
    }

    ////////////////// Step 5: Update state //////////////////
    for (int i = 0; i < this->lost_stracks.size(); i++) {
        if (this->frame_id - this->lost_stracks[i].end_frame() > this->max_time_lost) {
            this->lost_stracks[i].mark_removed();
            removed_stracks.push_back(this->lost_stracks[i]);
        }
    }

    for (int i = 0; i < this->tracked_stracks.size(); i++) {
        if (this->tracked_stracks[i].state == TrackState::Tracked) {
            tracked_stracks_swap.push_back(this->tracked_stracks[i]);
        }
    }
    this->tracked_stracks.clear();
    this->tracked_stracks.assign(tracked_stracks_swap.begin(), tracked_stracks_swap.end());

    this->tracked_stracks = joint_stracks(this->tracked_stracks, activated_stracks);
    this->tracked_stracks = joint_stracks(this->tracked_stracks, refind_stracks);

    //std::cout << activated_stracks.size() << std::endl;

    this->lost_stracks = sub_stracks(this->lost_stracks, this->tracked_stracks);
    for (int i = 0; i < lost_stracks.size(); i++) {
        this->lost_stracks.push_back(lost_stracks[i]);
    }

    this->lost_stracks = sub_stracks(this->lost_stracks, this->removed_stracks);
    for (int i = 0; i < removed_stracks.size(); i++) {
        this->removed_stracks.push_back(removed_stracks[i]);
    }

    remove_duplicate_stracks(resa, resb, this->tracked_stracks, this->lost_stracks);

    this->tracked_stracks.clear();
    this->tracked_stracks.assign(resa.begin(), resa.end());
    this->lost_stracks.clear();
    this->lost_stracks.assign(resb.begin(), resb.end());

    for (int i = 0; i < this->tracked_stracks.size(); i++) {
        if (this->tracked_stracks[i].is_activated) {
            output_stracks.push_back(this->tracked_stracks[i]);
        }
    }

    // clean up old objects
    vector<STrack > filtered_output_stracks;
    std::copy_if(output_stracks.begin(),
                 output_stracks.end(),
                 std::back_inserter(filtered_output_stracks),
                 [](STrack track) {
                     return track.associatedObjectIn != NULL &&
                            track.associatedObjectIn->classId == 0;
                 });
    return filtered_output_stracks;
}
