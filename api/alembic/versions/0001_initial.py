"""initial schema

Builds the full pipeline schema from SQLAlchemy metadata in one shot.

Revision ID: 0001_initial
Revises:
Create Date: 2026-05-06
"""
from alembic import op

revision = "0001_initial"
down_revision = None
branch_labels = None
depends_on = None


def upgrade() -> None:
    # Pull the live metadata from the models package and emit CREATE TABLE
    # for every registered table. This keeps the migration in lock-step
    # with models.py without having to hand-roll each definition.
    from api.models import Base
    bind = op.get_bind()
    Base.metadata.create_all(bind=bind)


def downgrade() -> None:
    from api.models import Base
    bind = op.get_bind()
    Base.metadata.drop_all(bind=bind)
