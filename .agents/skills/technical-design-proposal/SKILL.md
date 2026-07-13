---
name: technical-design-proposal
description: Generate technical design proposals using a structured template. Use when user wants to create a technical design document, RFC, design proposal, or mentions "technical scheme", "design document", "proposal template".
---

# Technical Design Proposal Skill

Generate comprehensive technical design proposals using a structured template with guided sections.

## Quick Start

1. User provides context: feature description, problem statement, or requirements
2. Skill guides through each section of the template
3. Output a complete technical design document

## Workflow

### Phase 1: Gather Context

Ask user for:
- [ ] Problem/feature description
- [ ] Target users and use cases
- [ ] Related Issue/PR numbers (if any)
- [ ] Any existing constraints or requirements

### Phase 2: Generate Document Sections

Follow the template structure, filling each section:

#### 1. Overview (概述)
- **Summary**: 1-2 paragraphs on core objectives and value
- **Motivation**: Current pain points, user value, impact of not doing
- **Goals/Non-goals**: Clear boundaries of scope

#### 2. Use Case Analysis (用例分析)
- Functional requirements
- Performance indicators
- Security/privacy requirements
- DFX requirements (compatibility, maintainability, testability, reliability)

#### 3. Design (方案设计)

**3.1 Overall Design**
- Architecture diagram (if complex)
- Core logic flow
- Platform/OS/programming model choices
- Constraints and limitations

**3.2 Technical Alternatives**
- Considered but rejected approaches
- Trade-off analysis

**3.3 Functional & Performance Design**
- Implementation details
- Data model changes
- Impact scope

**3.4 Security & DFX Design**
- Security considerations
- Privacy handling
- DFX attributes

**3.5 Programming Interface Design** (if applicable)
- Development environment
- API definitions with parameter tables
- Programming manual outline

#### 4. Risks & Drawbacks (缺点和风险)
- Breaking changes
- Performance regression risks
- Migration path for existing users

#### 5. Prior Art (现有技术)
- Similar designs in other projects
- What to borrow vs. differ

#### 6. Open Issues (未解决问题)
- Decisions pending community input
- Must resolve before RFC approval

### Phase 3: Review & Refine

- [ ] All sections completed
- [ ] Technical accuracy verified
- [ ] No placeholder text remaining
- [ ] Related Issue/PR linked

## Template Reference

See [REFERENCE.md](REFERENCE.md) for the complete template structure with section guidance.

## Examples

See [EXAMPLES.md](EXAMPLES.md) for sample outputs.

## Tips

- Start with motivation and goals before diving into technical details
- Use diagrams for complex architectures (sequence, activity, state machine)
- Be explicit about non-goals to prevent scope creep
- Include specific metrics for performance requirements
- Provide code examples for API sections
