## ADDED Requirements

### Requirement: Fetch target ownership SHALL expose decodeStartPC

The system SHALL track, for each fetch target, the earliest architectural
instruction-start PC that belongs to that target for decode/buildInst ownership.

#### Scenario: Ordinary target owns its natural start PC

- **GIVEN** a fetch target whose instructions do not migrate from a previous block
- **WHEN** the target is created
- **THEN** `decodeStartPC` SHALL equal the target `startPC`

#### Scenario: Following target claims a carried split control instruction

- **GIVEN** a 4-byte control-flow instruction starts at PC `P`
- **AND** its predictor-visible `controlPC = P + 2` belongs to the following fetch target
- **AND** that following target predicts the instruction taken
- **WHEN** the following target is created
- **THEN** its `decodeStartPC` SHALL be `P`

### Requirement: Fetch SHALL migrate split-control ownership before DynInst construction

The system SHALL switch to the owner fetch target before constructing a DynInst
for a control instruction whose decode ownership migrated to the following target.

#### Scenario: Cross-boundary taken control builds under the following target

- **GIVEN** the current instruction start PC is `P`
- **AND** the current fetch target is followed by another target whose `decodeStartPC == P`
- **WHEN** fetch processes the instruction at `P`
- **THEN** it SHALL consume the current target before `buildInst()`
- **AND** the constructed DynInst SHALL be associated with the following target

### Requirement: Taken redirect SHALL follow owner-target startPC matching

The system SHALL determine taken redirect eligibility from the owner target's
predicted control instruction start PC, without an extra trigger-coverage gate.

#### Scenario: Leading block does not redirect a carried split control

- **GIVEN** a split 4-byte control instruction starts at PC `P`
- **AND** the leading block does not own that instruction for decode
- **WHEN** fetch processes the leading block
- **THEN** it SHALL NOT redirect the branch as taken from that block

#### Scenario: Following block redirects when it owns the control instruction

- **GIVEN** a following fetch target owns a split control instruction starting at PC `P`
- **AND** that target predicts the instruction taken
- **WHEN** fetch constructs the instruction at `P` under that target
- **THEN** taken redirect SHALL trigger from that target
- **AND** the decision SHALL be based on matching `P` with the owner target's predicted branch start PC
