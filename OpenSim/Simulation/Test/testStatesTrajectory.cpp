/* -------------------------------------------------------------------------- *
 *                     OpenSim:  testStatesTrajectory.cpp                     *
 * -------------------------------------------------------------------------- *
 * The OpenSim API is a toolkit for musculoskeletal modeling and simulation.  *
 * See http://opensim.stanford.edu and the NOTICE file for more information.  *
 * OpenSim is developed at Stanford University and supported by the US        *
 * National Institutes of Health (U54 GM072970, R24 HD065690) and by DARPA    *
 * through the Warrior Web program.                                           *
 *                                                                            *
 * Copyright (c) 2005-2015 Stanford University and the Authors                *
 * Author(s): Chris Dembia                                                    *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may    *
 * not use this file except in compliance with the License. You may obtain a  *
 * copy of the License at http://www.apache.org/licenses/LICENSE-2.0.         *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,          *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 * -------------------------------------------------------------------------- */

#include <OpenSim/Simulation/osimSimulation.h>
#include <OpenSim/Common/Constant.h>
#include <OpenSim/Common/LoadOpenSimLibrary.h>
#include <random>
#include <cstdio>

using namespace OpenSim;
using namespace SimTK;

// TODO more informative "IndexOutOfRange" exception when using get().

// TODO detailed exceptions when integrity checks fail.
// TODO currently, one gets segfaults if state is not realized.
// TODO accessing acceleration-level outputs.

// TODO append two StateTrajectories together.
// TODO test modeling options (locked coordinates, etc.)
// TODO store a model within a StatesTrajectory.

const std::string statesStoFname = "testStatesTrajectory_readStorage_states.sto";
const std::string pre40StoFname = "std_subject01_walk1_states.sto";

// Helper function to get a state variable value from a storage file.
Real getStorageEntry(const Storage& sto,
        const int timeIndex, const std::string& columnName) {
    Real value;
    const int columnIndex = sto.getStateIndex(columnName);
    sto.getData(timeIndex, columnIndex, value);
    return value;
}

void testPopulateTrajectory() {
    Model model("gait2354_simbody.osim");

    // To assist with creating interesting (non-zero) coordinate values:
    model.updCoordinateSet().get("pelvis_ty").setDefaultLocked(true);

    auto& state = model.initSystem();

    SimTK::RungeKuttaMersonIntegrator integrator(model.getSystem());
    SimTK::TimeStepper ts(model.getSystem(), integrator);
    ts.initialize(state);
    ts.setReportAllSignificantStates(true);
    integrator.setReturnEveryInternalStep(true);

    StatesTrajectory states;
    const double finalTime = 0.05;
    std::vector<double> times;
    while (ts.getState().getTime() < finalTime) {
        ts.stepTo(finalTime);
        times.push_back(ts.getState().getTime());
        // StatesTrajectory API for appending states:
        states.append(ts.getState());
    }

    // Make sure we have all the states
    SimTK_TEST_EQ((int)states.getSize(), (int)times.size());
    // ...and that they aren't all just references to the same single state.
    for (int i = 0; i < states.getSize(); ++i) {
        SimTK_TEST_EQ(states[i].getTime(), times[i]);
    }

}

void testFrontBack() {
    Model model("arm26.osim");
    const auto& state = model.initSystem();
    StatesTrajectory states;
    states.append(state);
    states.append(state);
    states.append(state);

    SimTK_TEST(&states.front() == &states[0]);
    SimTK_TEST(&states.back() == &states[2]);
}

// Create states storage file to for states storage tests.
void createStateStorageFile() {

    Model model("gait2354_simbody.osim");

    // To assist with creating interesting (non-zero) coordinate values:
    model.updCoordinateSet().get("pelvis_ty").setDefaultLocked(true);

    // Randomly assign muscle excitations to create interesting activation
    // histories.
    auto* controller = new PrescribedController();
    // For consistent results, use same seed each time.
    std::default_random_engine generator(0); 
    // Uniform distribution between 0.1 and 0.9.
    std::uniform_real_distribution<double> distribution(0.1, 0.8);

    for (int im = 0; im < model.getMuscles().getSize(); ++im) {
        controller->addActuator(model.getMuscles()[im]);
        controller->prescribeControlForActuator(
            model.getMuscles()[im].getName(),
            new Constant(distribution(generator))
            );
    }

    model.addController(controller);

    auto& initState = model.initSystem();
    SimTK::RungeKuttaMersonIntegrator integrator(model.getSystem());
    Manager manager(model, integrator);
    manager.setFinalTime(0.15);
    manager.integrate(initState);
    manager.getStateStorage().print(statesStoFname);
}

void testFromStatesStorageGivesCorrectStates() {

    // Read in trajectory.
    // -------------------
    Model model("gait2354_simbody.osim");

    Storage sto(statesStoFname);

    // It's important that we have not yet initialized the model, 
    // since `createFromStatesStorage()` should be able to work with such a
    // model.
    auto states = StatesTrajectory::createFromStatesStorage(model, sto);

    // However, we eventually *do* need to call initSystem() to make use of the
    // trajectory with the model.
    model.initSystem();

    // Test that the states are correct, and also that the iterator works.
    // -------------------------------------------------------------------
    int itime = 0;
    double currTime;
    for (const auto& state : states) {
        // Time.
        sto.getTime(itime, currTime);
        SimTK_TEST_EQ(currTime, state.getTime());

        // Multibody states.
        for (int ic = 0; ic < model.getCoordinateSet().getSize(); ++ic) {
            const auto& coord = model.getCoordinateSet().get(ic);
            auto coordName = coord.getName();
            auto jointName = coord.getJoint().getName();
            auto coordPath = jointName + "/" + coordName;
            // Coordinate.
            SimTK_TEST_EQ(getStorageEntry(sto, itime, coordPath + "/value"),
                    coord.getValue(state));

            // Speed.
            SimTK_TEST_EQ(getStorageEntry(sto, itime, coordPath + "/speed"),
                    coord.getSpeedValue(state));
        }

        // Muscle states.
        for (int im = 0; im < model.getMuscles().getSize(); ++im) {
            const auto& muscle = model.getMuscles().get(im);
            auto muscleName = muscle.getName();

            // Activation.
            // TODO Simply accessing these state variables requires realizing
            // to Velocity; I think this is a bug.
            model.getMultibodySystem().realize(state, SimTK::Stage::Velocity);
            SimTK_TEST_EQ(
                    getStorageEntry(sto, itime, muscleName + "/activation"),
                    muscle.getActivation(state));

            // Fiber length.
            SimTK_TEST_EQ(
                    getStorageEntry(sto, itime, muscleName + "/fiber_length"), 
                    muscle.getFiberLength(state));

            // More complicated computation based on state.
            SimTK_TEST(!SimTK::isNaN(muscle.getFiberForce(state)));
        }

        // More complicated computations based on state.
        auto loc = model.getBodySet().get("tibia_r")
            .findLocationInAnotherFrame(state,
                    SimTK::Vec3(1, 0.5, 0.25),
                    model.getGround());
        SimTK_TEST(!loc.isNaN());

        SimTK_TEST(!model.calcMassCenterVelocity(state).isNaN());
        // TODO acceleration-level stuff gives segfault.
        // TODO model.getMultibodySystem().realize(state, SimTK::Stage::Acceleration);
        // TODO SimTK_TEST(!model.calcMassCenterAcceleration(state).isNaN());

        itime++;
    }
}

Storage newStorageWithRemovedRows(const Storage& origSto,
        const std::set<int>& rowsToRemove) {
    Storage sto(1000);
    auto labels = origSto.getColumnLabels();
    auto numOrigColumns = origSto.getColumnLabels().getSize() - 1;

    // Remove in reverse order so it's easier to keep track of indices.
     for (auto it = rowsToRemove.rbegin(); it != rowsToRemove.rend(); ++it) {
         labels.remove(*it);
     }
    sto.setColumnLabels(labels);

    double time;
    for (int itime = 0; itime < origSto.getSize(); ++itime) {
        SimTK::Vector rowData(numOrigColumns);
        origSto.getData(itime, numOrigColumns, rowData);

        SimTK::Vector newRowData(numOrigColumns - (int)rowsToRemove.size());
        int iNew = 0;
        for (int iOrig = 0; iOrig < numOrigColumns; ++iOrig) {
            if (rowsToRemove.count(iOrig) == 0) {
                newRowData[iNew] = rowData[iOrig];
                ++iNew;
            }
        }

        origSto.getTime(itime, time);
        sto.append(time, newRowData);
    }
    return sto;
}

void testFromStatesStorageInconsistentModel(const std::string &stoFilepath) {

    // States are missing from the Storage.
    // ------------------------------------
    {
        Model model("gait2354_simbody.osim");
        // Needed for getting state variable names.
        model.initSystem();

        const auto stateNames = model.getStateVariableNames();
        Storage sto(stoFilepath);
        // So the test doesn't take so long.
        sto.resampleLinear((sto.getLastTime() - sto.getFirstTime()) / 10);

        // Create new Storage with fewer columns.
        auto labels = sto.getColumnLabels();

        auto origLabel10 = stateNames[10];
        auto origLabel15 = stateNames[15];
        Storage stoMissingCols = newStorageWithRemovedRows(sto, {
                // gymnastics to be compatible with pre-v4.0 column names:
                sto.getStateIndex(origLabel10) + 1,
                sto.getStateIndex(origLabel15) + 1});

        // Test that an exception is thrown.
        SimTK_TEST_MUST_THROW_EXC(
                StatesTrajectory::createFromStatesStorage(model, stoMissingCols),
                StatesTrajectory::MissingColumnsInStatesStorage
                );
        // Check some other similar calls.
        SimTK_TEST_MUST_THROW_EXC(
                StatesTrajectory::createFromStatesStorage(model, stoMissingCols,
                    false, true),
                StatesTrajectory::MissingColumnsInStatesStorage
                );
        SimTK_TEST_MUST_THROW_EXC(
                StatesTrajectory::createFromStatesStorage(model, stoMissingCols,
                    false, false),
                StatesTrajectory::MissingColumnsInStatesStorage
                );

        // No exception if allowing missing columns.
        // The unspecified states are set to NaN (for at least two random
        // states).
        auto states = StatesTrajectory::createFromStatesStorage(
                model, stoMissingCols, true);
        SimTK_TEST(SimTK::isNaN(
                    model.getStateVariableValue(states[0], origLabel10)));
        SimTK_TEST(SimTK::isNaN(
                    model.getStateVariableValue(states[4], origLabel15)));
        // Behavior is independent of value for allowMissingColumns.
        StatesTrajectory::createFromStatesStorage(model, stoMissingCols,
                true, true);
        StatesTrajectory::createFromStatesStorage(model, stoMissingCols,
                true, false);
    }

    // States are missing from the Model.
    // ----------------------------------
    {
        Model model("gait2354_simbody.osim");
        Storage sto(stoFilepath);
        // So the test doesn't take so long.
        sto.resampleLinear((sto.getLastTime() - sto.getFirstTime()) / 10);

        // Remove a few of the muscles.
        model.updForceSet().remove(0);
        model.updForceSet().remove(10);
        model.updForceSet().remove(30);

        // Test that an exception is thrown.
        SimTK_TEST_MUST_THROW_EXC(
                StatesTrajectory::createFromStatesStorage(model, sto),
                StatesTrajectory::ExtraColumnsInStatesStorage
                );
        SimTK_TEST_MUST_THROW_EXC(
                StatesTrajectory::createFromStatesStorage(model, sto,
                    true, false),
                StatesTrajectory::ExtraColumnsInStatesStorage
                );
        SimTK_TEST_MUST_THROW_EXC(
                StatesTrajectory::createFromStatesStorage(model, sto,
                    false, false),
                StatesTrajectory::ExtraColumnsInStatesStorage
                );

        // No exception if allowing extra columns, and behavior is
        // independent of value for allowMissingColumns.
        StatesTrajectory::createFromStatesStorage(model, sto, true, true);
        StatesTrajectory::createFromStatesStorage(model, sto, false, true);
    }
}

void testFromStatesStorageUniqueColumnLabels() {

    Model model("gait2354_simbody.osim");
    Storage sto(statesStoFname);
    
    // Edit column labels so that they are not unique.
    auto labels = sto.getColumnLabels();
    labels[10] = labels[7];
    sto.setColumnLabels(labels); 
   
    SimTK_TEST_MUST_THROW_EXC(
            StatesTrajectory::createFromStatesStorage(model, sto),
            StatesTrajectory::NonUniqueColumnsInStatesStorage);
    SimTK_TEST_MUST_THROW_EXC(
            StatesTrajectory::createFromStatesStorage(model, sto, true, true),
            StatesTrajectory::NonUniqueColumnsInStatesStorage);
    SimTK_TEST_MUST_THROW_EXC(
            StatesTrajectory::createFromStatesStorage(model, sto, true, false),
            StatesTrajectory::NonUniqueColumnsInStatesStorage);
    SimTK_TEST_MUST_THROW_EXC(
            StatesTrajectory::createFromStatesStorage(model, sto, false, true),
            StatesTrajectory::NonUniqueColumnsInStatesStorage);
    SimTK_TEST_MUST_THROW_EXC(
            StatesTrajectory::createFromStatesStorage(model, sto, false, false),
            StatesTrajectory::NonUniqueColumnsInStatesStorage);

    // TODO unique even considering old and new formats for state variable
    // names (/value and /speed) in the same file.
}

void testFromStatesStoragePre40CorrectStates() {
    // This test is very similar to testFromStatesStorageGivesCorrectStates
    // TODO could avoid the duplicate function since getStateIndex handles
    // pre-4.0 names.

    // Read in trajectory.
    // -------------------
    Model model("gait2354_simbody.osim");

    Storage sto(pre40StoFname);
    // So the test doesn't take so long.
    sto.resampleLinear(0.01);
    auto states = StatesTrajectory::createFromStatesStorage(model, sto);

    model.initSystem();

    // Test that the states are correct.
    // ---------------------------------
    int itime = 0;
    double currTime;
    for (const auto& state : states) {
        // Time.
        sto.getTime(itime, currTime);
        SimTK_TEST_EQ(currTime, state.getTime());

        // Multibody states.
        for (int ic = 0; ic < model.getCoordinateSet().getSize(); ++ic) {
            const auto& coord = model.getCoordinateSet().get(ic);
            auto coordName = coord.getName();

            // Coordinate.
            SimTK_TEST_EQ(getStorageEntry(sto, itime, coordName),
                    coord.getValue(state));

            // Speed.
            SimTK_TEST_EQ(getStorageEntry(sto, itime, coordName + "_u"),
                    coord.getSpeedValue(state));
        }

        // Muscle states.
        for (int im = 0; im < model.getMuscles().getSize(); ++im) {
            const auto& muscle = model.getMuscles().get(im);
            auto muscleName = muscle.getName();

            // Activation.
            // TODO Simply accessing these state variables requires realizing
            // to Velocity; I think this is a bug.
            model.getMultibodySystem().realize(state, SimTK::Stage::Velocity);
            SimTK_TEST_EQ(
                    getStorageEntry(sto, itime, muscleName + ".activation"),
                    muscle.getActivation(state));

            // Fiber length.
            SimTK_TEST_EQ(
                    getStorageEntry(sto, itime, muscleName + ".fiber_length"), 
                    muscle.getFiberLength(state));

            // More complicated computation based on state.
            SimTK_TEST(!SimTK::isNaN(muscle.getFiberForce(state)));
        }

        // More complicated computations based on state.
        auto loc = model.getBodySet().get("tibia_r")
            .findLocationInAnotherFrame(state,
                    SimTK::Vec3(1, 0.5, 0.25),
                    model.getGround());
        SimTK_TEST(!loc.isNaN());

        SimTK_TEST(!model.calcMassCenterVelocity(state).isNaN());
        // TODO acceleration-level stuff gives segfault.
        // TODO model.getMultibodySystem().realize(state, SimTK::Stage::Acceleration);
        // TODO SimTK_TEST(!model.calcMassCenterAcceleration(state).isNaN());

        itime++;
    }
}

void testFromStatesStorageAllRowsHaveSameLength() {
    // Missing data in individual rows leads to an incorrect StatesTrajectory
    // and could really confuse users.
    Model model("gait2354_simbody.osim");
    model.initSystem();

    const auto stateNames = model.getStateVariableNames();
    Storage sto(statesStoFname);
    // Append a too-short state vector.
    std::vector<double> v(model.getNumStateVariables() - 10, 1.0);
    StateVector sv(25.0, model.getNumStateVariables() - 10, v.data());
    sto.append(sv);

    SimTK_TEST_MUST_THROW_EXC(
            StatesTrajectory::createFromStatesStorage(model, sto),
            StatesTrajectory::VaryingNumberOfStatesPerRow);
}


void testCopying() {
    Model model("gait2354_simbody.osim");
    auto& state = model.initSystem();

    StatesTrajectory states;
    {
        state.setTime(0.5);
        states.append(state);
        state.setTime(1.3);
        states.append(state);
        state.setTime(3.5);
        states.append(state);
    }

    {
        StatesTrajectory statesCopyConstruct(states);
        // Ideally we'd check for equality (operator==()), but State does not
        // have an equality operator.
        SimTK_TEST_EQ((int)statesCopyConstruct.getSize(), (int)states.getSize());
        for (int i = 0; i < states.getSize(); ++i) {
            SimTK_TEST_EQ(statesCopyConstruct[i].getTime(), states[i].getTime());
        }
    }

    {
        StatesTrajectory statesCopyAssign;
        statesCopyAssign = states;
        SimTK_TEST_EQ((int)statesCopyAssign.getSize(), (int)states.getSize());
        for (int i = 0; i < states.getSize(); ++i) {
            SimTK_TEST_EQ(statesCopyAssign[i].getTime(), states[i].getTime());
        }
    }
}

void testAccessByTime() {
    Model model("gait2354_simbody.osim");
    auto& state = model.initSystem();

    // Make a trajectory with different times.
    StatesTrajectory states;
    state.setTime(0.5);
    states.append(state); // 0
    state.setTime(1.3);
    states.append(state); // 1
    state.setTime(3.5);
    states.append(state); // 2
    state.setTime(4.1);
    states.append(state); // 3
    
    // Nearest before.
    SimTK_TEST(&states.getNearestBefore(0.5) == &states[0]);
    SimTK_TEST(&states.getNearestBefore(0.5000001) == &states[0]);
    SimTK_TEST(&states.getNearestBefore(1.299999999) == &states[0]);

    SimTK_TEST(&states.getNearestBefore(1.3) == &states[1]);
    SimTK_TEST(&states.getNearestBefore(1.300001) == &states[1]);
    SimTK_TEST(&states.getNearestBefore(3.49999) == &states[1]);

    SimTK_TEST(&states.getNearestBefore(4.1) == &states[3]);
    SimTK_TEST(&states.getNearestBefore(4.2) == &states[3]);
    SimTK_TEST(&states.getNearestBefore(153.7) == &states[3]);

    // Nearest after.
    SimTK_TEST(&states.getNearestAfter(-100.0) == &states[0]);
    SimTK_TEST(&states.getNearestAfter(0) == &states[0]);
    SimTK_TEST(&states.getNearestAfter(0.1) == &states[0]);
    SimTK_TEST(&states.getNearestAfter(0.4999999999) == &states[0]);
    SimTK_TEST(&states.getNearestAfter(0.5) == &states[0]);

    SimTK_TEST(&states.getNearestAfter(1.3000000000001) == &states[2]);
    SimTK_TEST(&states.getNearestAfter(1.31) == &states[2]);
    SimTK_TEST(&states.getNearestAfter(1.4) == &states[2]);
    SimTK_TEST(&states.getNearestAfter(3.4) == &states[2]);
    SimTK_TEST(&states.getNearestAfter(3.4999999999999) == &states[2]);
    SimTK_TEST(&states.getNearestAfter(3.5) == &states[2]);
    
    SimTK_TEST(&states.getNearestAfter(4.0001) == &states[3]);
    SimTK_TEST(&states.getNearestAfter(4.1) == &states[3]);

    // Exception if there is not a state before/after the specified time.
    SimTK_TEST_MUST_THROW_EXC(states.getNearestBefore(0.25),
            StatesTrajectory::TimeOutOfRange);
    SimTK_TEST_MUST_THROW_EXC(states.getNearestBefore(0.0),
            StatesTrajectory::TimeOutOfRange);
    SimTK_TEST_MUST_THROW_EXC(states.getNearestBefore(-1.0),
            StatesTrajectory::TimeOutOfRange);
    SimTK_TEST_MUST_THROW_EXC(states.getNearestBefore(.499999),
            StatesTrajectory::TimeOutOfRange);

    SimTK_TEST_MUST_THROW_EXC(states.getNearestAfter(4.10000001),
            StatesTrajectory::TimeOutOfRange);
    SimTK_TEST_MUST_THROW_EXC(states.getNearestAfter(1050.9),
            StatesTrajectory::TimeOutOfRange);

    // Simulate the scenario that the tolerance is intended to handle.
    // TODO
    // TODO SimTK_TEST(&states.getNearestBefore(1.299, 0.001) == &states[1]);

    // Using a custom tolerance.
    SimTK_TEST(&states.getNearestBefore(0.499, 0.001) == &states[0]);
    SimTK_TEST(&states.getNearestBefore(0.495, 0.005) == &states[0]);
    SimTK_TEST(&states.getNearestBefore(1.2991, 0.001) == &states[1]);
    SimTK_TEST(&states.getNearestBefore(1.298999, 0.001) == &states[0]);
    SimTK_TEST(&states.getNearestBefore(4.099, 0.001) == &states[3]);
    SimTK_TEST(&states.getNearestBefore(4.098, 0.001) == &states[2]);

    SimTK_TEST(&states.getNearestAfter(0.501, 0.001) == &states[0]);
    SimTK_TEST(&states.getNearestAfter(0.503, 0.003) == &states[0]);
    // Past the tolerance, we get the next state.
    SimTK_TEST(&states.getNearestAfter(0.50301, 0.003) == &states[1]);
    SimTK_TEST(&states.getNearestAfter(1.301, 0.001) == &states[1]);
    SimTK_TEST(&states.getNearestAfter(1.301001, 0.001) == &states[2]);
    SimTK_TEST(&states.getNearestAfter(4.1005, 0.001) == &states[3]);
    SimTK_TEST(&states.getNearestAfter(4.101, 0.001) == &states[3]);

    // Test exceptions with custom tolerance.
    SimTK_TEST_MUST_THROW_EXC(states.getNearestBefore(0.498, 0.001),
            StatesTrajectory::TimeOutOfRange);
    SimTK_TEST_MUST_THROW_EXC(states.getNearestBefore(0.498999, 0.001),
            StatesTrajectory::TimeOutOfRange);
    SimTK_TEST_MUST_THROW_EXC(states.getNearestAfter(4.102, 0.001),
            StatesTrajectory::TimeOutOfRange);
    SimTK_TEST_MUST_THROW_EXC(states.getNearestAfter(4.10100001, 0.001),
            StatesTrajectory::TimeOutOfRange);

    // Make sure that we get the right state when there are multiple with
    // the same time.
    // TODO
    states.append(state); // 4. This state has time 4.1.
    states.append(state); // 5
    state.setTime(5.8);
    states.append(state); // 6
    states.append(state); // 7
    SimTK_TEST(&states.getNearestAfter(4.0) == &states[3]);
    SimTK_TEST(&states.getNearestAfter(4.1) == &states[3]);
    SimTK_TEST(&states.getNearestBefore(5.0) == &states[5]);
    SimTK_TEST(&states.getNearestBefore(4.1) == &states[5]);

    SimTK_TEST(&states.getNearestAfter(4.1004999, 0.0005) == &states[3]);
    SimTK_TEST(&states.getNearestBefore(4.0995, 0.0005) == &states[5]);

    SimTK_TEST(&states.getNearestAfter(5.8) == &states[6]);
    SimTK_TEST(&states.getNearestBefore(5.8) == &states[7]);
    SimTK_TEST_MUST_THROW_EXC(states.getNearestAfter(5.800001),
            StatesTrajectory::TimeOutOfRange);

    // TODO also check behavior when multiple states all fall within a
    // "SignificantReal" window, or fall in whatever the tolerance is.
    //
    // TODO check that we get the right behavior when the time IS a number that
    // can be exactly represented as a float.

    // Get a container view.
    // TODO make a new StatesTrajectoryView class, or use SimTK::Array_
    // TODO uh oh...might want to make the class compatible, and then introduce
    // it into the inheritance hierarchy...

    // Or just provide iterators?
}

/*
SimTK::State does not have an equality operator, so we can't test equality of
two StatesTrajectory's yet.
void testEqualityOperator() {
    // Test trajectories that hold a single state.
    {
        Model model("gait2354_simbody.osim");
        const auto& state = model.initSystem();

        StatesTrajectory statesA;
        statesA.append(state);

        StatesTrajectory statesB;
        statesB.append(state);

        SimTK_TEST(statesA == statesB);

        // Ensure that two different trajectories are not equal.
        // -----------------------------------------------------
        {
            SimTK::State differentState(state);
            differentState.setTime(53.67);

            StatesTrajectory differentStates;
            differentStates.append(differentState);

            SimTK_TEST(states != differentStates);
        }

        // Copy constructor.
        // -----------------
        StatesTrajectory statesB2(statesB);
        SimTK_TEST(statesB2 == statesB);

        // Copy assignment.
        // ----------------
        StatesTrajectory statesB3 = statesB;
        SimTK_TEST(statesB3 == statesB);
        // TODO ensure two non-equal copied states come up as such.

    }
    
    // TODO auto state1 = SimTK::State(state);
    // TODO state1.setTime(
    // TODO auto state2 = SimTK::State(state1);
    // TODO better testing of copy constructor.
}
*/

void testAppendTimesAreNonDecreasing() {
    Model model("gait2354_simbody.osim");
    auto& state = model.initSystem();
    state.setTime(1.0);

    StatesTrajectory states;
    states.append(state);

    // Multiple states can have the same time; does not throw an exception:
    states.append(state);

    state.setTime(0.9999);
    SimTK_TEST_MUST_THROW_EXC(states.append(state),
            SimTK::Exception::APIArgcheckFailed);
}

void testBoundsCheck() {
    Model model("gait2354_simbody.osim");
    const auto& state = model.initSystem();
    StatesTrajectory states;
    states.append(state);
    states.append(state);
    states.append(state);
    states.append(state);
    
    states[states.getSize() + 100];
    states[4];
    states[5];
    SimTK_TEST_MUST_THROW_EXC(states.get(4), std::out_of_range);
    SimTK_TEST_MUST_THROW_EXC(states.get(states.getSize() + 100),
            std::out_of_range);
}

void testIntegrityChecks() {
    Model arm26("arm26.osim");
    const auto& s26 = arm26.initSystem();

    Model gait2354("gait2354_simbody.osim");
    const auto& s2354 = gait2354.initSystem();
    // TODO add models with events, unilateral constraints, etc.

    // Times are nondecreasing.
    {
        StatesTrajectory states;
        auto state0(s26);
        state0.setTime(0.5);
        auto state1(s26);
        state1.setTime(0.6);

        states.append(state0);
        states.append(state1);

        SimTK_TEST(states.isConsistent());
        SimTK_TEST(states.isNondecreasingInTime());
        SimTK_TEST(states.hasIntegrity());

        // Users should never do this const cast; it's just for the sake of
        // the test.
        const_cast<SimTK::State*>(&states[1])->setTime(0.2);

        SimTK_TEST(states.isConsistent());
        SimTK_TEST(!states.isNondecreasingInTime());
        SimTK_TEST(!states.hasIntegrity());
    }

    // Consistency and compatibility with a model.
    {
        StatesTrajectory states;
        // An empty trajectory is consistent.
        SimTK_TEST(states.isConsistent());

        // A length-1 trajectory is consistent.
        states.append(s26);
        SimTK_TEST(states.isConsistent());

        // This trajectory is compatible with the arm26 model.
        SimTK_TEST(states.isCompatibleWith(arm26));

        // Not compatible with gait2354 model.
        // Ensures a lower-dimensional trajectory can't pass for a higher
        // dimensional model.
        SimTK_TEST(!states.isCompatibleWith(gait2354));

        // The checks still work with more than 1 state.
        states.append(s26);
        states.append(s26);
        SimTK_TEST(states.isNondecreasingInTime());
        SimTK_TEST(states.isConsistent());
        SimTK_TEST(states.hasIntegrity());
        SimTK_TEST(states.isCompatibleWith(arm26));
        SimTK_TEST(!states.isCompatibleWith(gait2354));
    }

    {
        StatesTrajectory states;
        states.append(s2354);

        // Reverse of the previous check; to ensure that a larger-dimensional
        // trajectory can't pass for the smaller dimensional model.
        SimTK_TEST(states.isCompatibleWith(gait2354));
        SimTK_TEST(!states.isCompatibleWith(arm26));

        // Check still works with more than 1 state.
        states.append(s2354);
        states.append(s2354);
        SimTK_TEST(states.isNondecreasingInTime());
        SimTK_TEST(states.isConsistent());
        SimTK_TEST(states.hasIntegrity());
        SimTK_TEST(states.isCompatibleWith(gait2354));
        SimTK_TEST(!states.isCompatibleWith(arm26));
    }

    {
        // Cannot append inconsistent states.
        StatesTrajectory states;
        states.append(s26);
        SimTK_TEST_MUST_THROW_EXC(states.append(s2354),
                StatesTrajectory::InconsistentState);

        // Same check, but swap the models.
        StatesTrajectory states2;
        states2.append(s2354);
        SimTK_TEST_MUST_THROW_EXC(states2.append(s26),
                StatesTrajectory::InconsistentState);
    }

    // TODO Show weakness of the test: two models with the same number of Q's, U's,
    // and Z's both pass the check. 
}


int main() {
    SimTK_START_TEST("testStatesTrajectory");
    
        // actuators library is not loaded automatically (unless using clang).
        #if !defined(__clang__)
            LoadOpenSimLibrary("osimActuators");
        #endif

        // Make sure the states Storage file doesn't already exist; we'll
        // generate it later and we don't want to use a stale one by accident.
        remove(statesStoFname.c_str());

        SimTK_SUBTEST(testPopulateTrajectory);
        SimTK_SUBTEST(testFrontBack);
        SimTK_SUBTEST(testBoundsCheck);
        SimTK_SUBTEST(testIntegrityChecks);
        SimTK_SUBTEST(testAppendTimesAreNonDecreasing);
        SimTK_SUBTEST(testCopying);
        SimTK_SUBTEST(testAccessByTime);

        // Test creation of trajectory from astates storage.
        // -------------------------------------------------
        // Using a pre-4.0 states storage file with old column names.
        SimTK_SUBTEST(testFromStatesStoragePre40CorrectStates);
        SimTK_SUBTEST1(testFromStatesStorageInconsistentModel, pre40StoFname);

        // v4.0 states storage
        createStateStorageFile();
        SimTK_SUBTEST(testFromStatesStorageGivesCorrectStates);
        SimTK_SUBTEST1(testFromStatesStorageInconsistentModel, statesStoFname);
        SimTK_SUBTEST(testFromStatesStorageUniqueColumnLabels);
        // TODO SimTK_SUBTEST(testEqualityOperator);
        SimTK_SUBTEST(testFromStatesStorageAllRowsHaveSameLength);

    SimTK_END_TEST();
}
