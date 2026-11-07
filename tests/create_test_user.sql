INSERT INTO public.users (name)
VALUES (:'user_name')
RETURNING id AS user_id
\gset

\echo Created :user_name with ID :user_id