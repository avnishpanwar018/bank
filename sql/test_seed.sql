USE bank_test;

INSERT INTO customers (name, email)
WITH RECURSIVE seed_numbers AS (
    SELECT 1 AS number
    UNION ALL
    SELECT number + 1 FROM seed_numbers WHERE number < 50
)
SELECT CONCAT('Seed Customer ', LPAD(number, 2, '0')),
       CONCAT('bank-test-seed-', LPAD(number, 2, '0'), '@example.test')
FROM seed_numbers
ON DUPLICATE KEY UPDATE name = VALUES(name);

INSERT INTO accounts (customer_id, balance)
SELECT customer_id, 100000.00
FROM customers
WHERE email LIKE 'bank-test-seed-%@example.test'
  AND NOT EXISTS (
      SELECT 1
      FROM accounts existing
      WHERE existing.customer_id = customers.customer_id
  );
