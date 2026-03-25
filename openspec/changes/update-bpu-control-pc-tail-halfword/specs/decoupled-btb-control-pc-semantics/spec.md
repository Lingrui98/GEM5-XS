## ADDED Requirements

### Requirement: BPU internal control-flow identity SHALL use controlPC

The system SHALL represent a control-flow instruction inside the decoupled BPU with a predictor-visible
`controlPC`, defined as the start address of the final 2-byte slice of that instruction.

#### Scenario: 4B RVI control instruction uses the tail-halfword PC

- **GIVEN** a 4-byte RISC-V control-flow instruction starts at PC `P`
- **WHEN** the instruction is represented inside predictor-visible BTB / FTQ / branch-info state
- **THEN** its predictor-visible `controlPC` SHALL be `P + 2`

#### Scenario: 2B RVC control instruction keeps the same PC

- **GIVEN** a 2-byte RISC-V compressed control-flow instruction starts at PC `P`
- **WHEN** the instruction is represented inside predictor-visible BPU state
- **THEN** its predictor-visible `controlPC` SHALL be `P`

### Requirement: Predictor indexing, tagging, and slot ownership SHALL follow controlPC

The system SHALL use `controlPC`, not `startPC`, for predictor-side index/tag/position calculations,
lookup matching, update matching, and block ownership of control-flow instructions.

#### Scenario: Split 4B control instruction belongs to the next prediction block

- **GIVEN** a 4-byte control-flow instruction starts at PC `P`
- **AND** its `controlPC = P + 2` lies in the next predict block
- **WHEN** the predictor decides which block owns the control-flow slot
- **THEN** the owning block SHALL be the block containing `controlPC`
- **AND** the previous block SHALL NOT claim the instruction as its taken control slot

#### Scenario: Training and lookup agree on the same controlPC

- **GIVEN** a control-flow instruction has been inserted or trained using predictor-visible branch state
- **WHEN** the same static instruction is looked up again later
- **THEN** the predictor SHALL use the same `controlPC`-derived keying rules for both update and lookup

### Requirement: Fetch coverage SHALL remain startPC-based and SHALL not redirect before trailing bytes are available

The system SHALL continue to use `startPC` and full-instruction byte coverage for fetch/decode assembly,
even when predictor ownership has moved to `controlPC`.

#### Scenario: Cross-boundary 4B control does not redirect on the leading halfword

- **GIVEN** a 4-byte control-flow instruction starts at PC `P`
- **AND** only the leading 2 bytes are available in the current fetch block
- **WHEN** the frontend evaluates whether to redirect as taken
- **THEN** it SHALL NOT redirect from that leading-halfword-only state
- **AND** it SHALL continue fetching until the trailing 2 bytes become available

#### Scenario: Same-block 4B control may redirect after full instruction coverage

- **GIVEN** a 4-byte control-flow instruction starts at PC `P`
- **AND** the current fetch stream already covers the full byte range `[P, P + 4)`
- **WHEN** the instruction is decoded and reaches its predictor-owned `controlPC`
- **THEN** the frontend MAY perform the taken redirect from that same block

### Requirement: Decoder byte fill SHALL support 2-byte incremental assembly

The system SHALL allow a single RISC-V instruction to be assembled from one or more 2-byte fills,
instead of requiring fetch to present 4 contiguous bytes before every decode attempt.

#### Scenario: Compressed instruction is ready after one halfword

- **GIVEN** a 2-byte compressed instruction starts at PC `P`
- **WHEN** the decoder receives the first 2-byte fill for that instruction
- **THEN** the decoder SHALL mark that instruction ready without requiring an additional fill

#### Scenario: 32-bit instruction waits for a second halfword

- **GIVEN** a 4-byte instruction starts at PC `P`
- **WHEN** the decoder receives only the first 2-byte fill for that instruction
- **THEN** the decoder SHALL keep that instruction not-ready
- **AND** it SHALL require a second 2-byte fill before decode completes

### Requirement: Range-sensitive debug output SHALL expose both startPC and controlPC

The system SHALL make both views observable in predictor/fetch debug paths so boundary-sensitive witnesses
can explain why a split control-flow instruction was owned by one block but assembled from another.

#### Scenario: Boundary witness prints both PC views

- **GIVEN** a control-flow instruction spans a predict or fetch boundary
- **WHEN** the frontend emits boundary-sensitive debug output for that event
- **THEN** the output SHALL include the instruction `startPC`
- **AND** the output SHALL include the predictor-visible `controlPC`
